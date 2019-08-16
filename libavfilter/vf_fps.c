/*
 * Copyright 2007 Bobby Bingham
 * Copyright 2012 Robert Nagy <ronag89 gmail com>
 * Copyright 2012 Anton Khirnov <anton khirnov net>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * a filter enforcing given constant framerate
 */

#include <float.h>
#include <stdint.h>

#include "libavutil/common.h"
#include "libavutil/fifo.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"

#define FF_INTERNAL_FIELDS 1
#include "framequeue.h"
#include "avfilter.h"
#include "internal.h"
#include "video.h"

enum EOFAction {
    EOF_ACTION_ROUND,
    EOF_ACTION_PASS,
    EOF_ACTION_NB
};

typedef struct FPSContext {
    const AVClass *class;

    AVFifoBuffer *fifo;     ///< store frames until we get two successive timestamps

    /* timestamps in input timebase */
    int64_t first_pts;      ///< pts of the first frame that arrived on this filter

    double start_time;      ///< pts, in seconds, of the expected first frame

    AVRational framerate;   ///< target framerate
    int rounding;           ///< AVRounding method for timestamps
    int eof_action;         ///< action performed for last frame in FIFO

    AVFifoBuffer *cc_608_fifo;
    AVFifoBuffer *cc_708_fifo;
    int expected_cc_count;
    int expected_608;
    int cc_detected;

    /* statistics */
    int frames_in;             ///< number of frames on input
    int frames_out;            ///< number of frames on output
    int dup;                   ///< number of frames duplicated
    int drop;                  ///< number of framed dropped
} FPSContext;

#define OFFSET(x) offsetof(FPSContext, x)
#define V AV_OPT_FLAG_VIDEO_PARAM
#define F AV_OPT_FLAG_FILTERING_PARAM
static const AVOption fps_options[] = {
    { "fps", "A string describing desired output framerate", OFFSET(framerate), AV_OPT_TYPE_VIDEO_RATE, { .str = "25" }, 0, INT_MAX, V|F },
    { "start_time", "Assume the first PTS should be this value.", OFFSET(start_time), AV_OPT_TYPE_DOUBLE, { .dbl = DBL_MAX}, -DBL_MAX, DBL_MAX, V|F },
    { "round", "set rounding method for timestamps", OFFSET(rounding), AV_OPT_TYPE_INT, { .i64 = AV_ROUND_NEAR_INF }, 0, 5, V|F, "round" },
        { "zero", "round towards 0",                 0, AV_OPT_TYPE_CONST, { .i64 = AV_ROUND_ZERO     }, 0, 0, V|F, "round" },
        { "inf",  "round away from 0",               0, AV_OPT_TYPE_CONST, { .i64 = AV_ROUND_INF      }, 0, 0, V|F, "round" },
        { "down", "round towards -infty",            0, AV_OPT_TYPE_CONST, { .i64 = AV_ROUND_DOWN     }, 0, 0, V|F, "round" },
        { "up",   "round towards +infty",            0, AV_OPT_TYPE_CONST, { .i64 = AV_ROUND_UP       }, 0, 0, V|F, "round" },
        { "near", "round to nearest",                0, AV_OPT_TYPE_CONST, { .i64 = AV_ROUND_NEAR_INF }, 0, 0, V|F, "round" },
    { "eof_action", "action performed for last frame", OFFSET(eof_action), AV_OPT_TYPE_INT, { .i64 = EOF_ACTION_ROUND }, 0, EOF_ACTION_NB-1, V|F, "eof_action" },
        { "round", "round similar to other frames",  0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_ROUND }, 0, 0, V|F, "eof_action" },
        { "pass",  "pass through last frame",        0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_PASS  }, 0, 0, V|F, "eof_action" },
    { NULL }
};

AVFILTER_DEFINE_CLASS(fps);

#define MAX_CC_ELEMENTS 128
#define CC_BYTES_PER_ENTRY 3

struct cc_lookup {
    int num;
    int den;
    int cc_count;
    int num_608;
};

const static struct cc_lookup cc_lookup_vals[] = {
    { 15, 1, 40, 4 },
    { 30, 1, 20, 2 },
    { 30000, 1001, 20, 2},
    { 60, 1, 10, 1 },
    { 60000, 1001, 10, 1},
};

static av_cold int init(AVFilterContext *ctx)
{
    FPSContext *s = ctx->priv;
    int i;

    if (!(s->fifo = av_fifo_alloc_array(2, sizeof(AVFrame*))))
        return AVERROR(ENOMEM);

    if (!(s->cc_708_fifo = av_fifo_alloc_array(MAX_CC_ELEMENTS, CC_BYTES_PER_ENTRY)))
        return AVERROR(ENOMEM);

    if (!(s->cc_608_fifo = av_fifo_alloc_array(MAX_CC_ELEMENTS, CC_BYTES_PER_ENTRY)))
        return AVERROR(ENOMEM);

    s->first_pts    = AV_NOPTS_VALUE;

    /* Based on the target FPS, figure out the expected cc_count and number of
       608 tuples per packet.  See ANSI/CTA-708-E Sec 4.3.6.1. */
    for (i = 0; i < (sizeof(cc_lookup_vals) / sizeof(struct cc_lookup)); i++) {
        if (s->framerate.num == cc_lookup_vals[i].num &&
            s->framerate.den == cc_lookup_vals[i].den) {
            s->expected_cc_count = cc_lookup_vals[i].cc_count;
            s->expected_608 = cc_lookup_vals[i].num_608;
            break;
        }
    }

    if (s->expected_608 == 0) {
        av_log(ctx, AV_LOG_WARNING, "fps cannot transcode captions fps=%d/%d\n", s->framerate.num, s->framerate.den);
    }

    av_log(ctx, AV_LOG_VERBOSE, "fps=%d/%d\n", s->framerate.num, s->framerate.den);
    return 0;
}

static void flush_fifo(AVFifoBuffer *fifo)
{
    while (av_fifo_size(fifo)) {
        AVFrame *tmp;
        av_fifo_generic_read(fifo, &tmp, sizeof(tmp), NULL);
        av_frame_free(&tmp);
    }
}

static av_cold void uninit(AVFilterContext *ctx)
{
    FPSContext *s = ctx->priv;
    if (s->fifo) {
        s->drop += av_fifo_size(s->fifo) / sizeof(AVFrame*);
        flush_fifo(s->fifo);
        av_fifo_freep(&s->fifo);
    }
    if (s->cc_608_fifo) {
        flush_fifo(s->cc_608_fifo);
        av_fifo_freep(&s->cc_608_fifo);
    }
    if (s->cc_708_fifo) {
        flush_fifo(s->cc_708_fifo);
        av_fifo_freep(&s->cc_708_fifo);
    }

    av_log(ctx, AV_LOG_VERBOSE, "%d frames in, %d frames out; %d frames dropped, "
           "%d frames duplicated.\n", s->frames_in, s->frames_out, s->drop, s->dup);
}

static int config_props(AVFilterLink* link)
{
    FPSContext   *s = link->src->priv;

    link->time_base = av_inv_q(s->framerate);
    link->frame_rate= s->framerate;
    link->w         = link->src->inputs[0]->w;
    link->h         = link->src->inputs[0]->h;

    return 0;
}

static void handle_frame_cc(AVFilterContext *ctx, AVFrame *buf)
{
    FPSContext        *s = ctx->priv;
    AVFrameSideData *sd;
    int cc_filled = 0;
    int i;

    if (s->cc_detected == 0 || s->expected_cc_count == 0)
        return;

    sd = av_frame_new_side_data(buf, AV_FRAME_DATA_A53_CC,
                                s->expected_cc_count * CC_BYTES_PER_ENTRY);
    if (!sd)
        return;

    for (i = 0; i < s->expected_608; i++) {
        if (av_fifo_size(s->cc_608_fifo) >= CC_BYTES_PER_ENTRY) {
            av_fifo_generic_read(s->cc_608_fifo, &sd->data[cc_filled * CC_BYTES_PER_ENTRY],
                                 CC_BYTES_PER_ENTRY, NULL);
            cc_filled++;
        } else {
            break;
        }
    }

    /* Insert any available data from the 708 FIFO */
    while (cc_filled < s->expected_cc_count) {
        if (av_fifo_size(s->cc_708_fifo) >= CC_BYTES_PER_ENTRY) {
            av_fifo_generic_read(s->cc_708_fifo, &sd->data[cc_filled * CC_BYTES_PER_ENTRY],
                                 CC_BYTES_PER_ENTRY, NULL);
            cc_filled++;
        } else {
            break;
        }
    }

    /* Insert 708 padding into any remaining fields */
    while (cc_filled < s->expected_cc_count) {
        sd->data[cc_filled * CC_BYTES_PER_ENTRY]     = 0xfa;
        sd->data[cc_filled * CC_BYTES_PER_ENTRY + 1] = 0x00;
        sd->data[cc_filled * CC_BYTES_PER_ENTRY + 2] = 0x00;
        cc_filled++;
    }
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    FPSContext        *s = ctx->priv;
    int ret;

    ret = ff_request_frame(ctx->inputs[0]);

    /* flush the fifo */
    if (ret == AVERROR_EOF && av_fifo_size(s->fifo)) {
        int i;
        for (i = 0; av_fifo_size(s->fifo); i++) {
            AVFrame *buf;

            av_fifo_generic_read(s->fifo, &buf, sizeof(buf), NULL);
            if (av_fifo_size(s->fifo)) {
                buf->pts = av_rescale_q(s->first_pts, ctx->inputs[0]->time_base,
                                        outlink->time_base) + s->frames_out;
                handle_frame_cc(ctx, buf);

                if ((ret = ff_filter_frame(outlink, buf)) < 0)
                    return ret;

                s->frames_out++;
            } else {
                /* This is the last frame, we may have to duplicate it to match
                 * the last frame duration */
                int j;
                int eof_rounding = (s->eof_action == EOF_ACTION_PASS) ? AV_ROUND_UP : s->rounding;
                int delta = av_rescale_q_rnd(ctx->inputs[0]->current_pts - s->first_pts,
                                             ctx->inputs[0]->time_base,
                                             outlink->time_base, eof_rounding) - s->frames_out;
                av_log(ctx, AV_LOG_DEBUG, "EOF frames_out:%d delta:%d\n", s->frames_out, delta);
                /* if the delta is equal to 1, it means we just need to output
                 * the last frame. Greater than 1 means we will need duplicate
                 * delta-1 frames */
                if (delta > 0 ) {
                    for (j = 0; j < delta; j++) {
                        AVFrame *dup = av_frame_clone(buf);

                        av_log(ctx, AV_LOG_DEBUG, "Duplicating frame.\n");
                        dup->pts = av_rescale_q(s->first_pts, ctx->inputs[0]->time_base,
                                                outlink->time_base) + s->frames_out;
                        handle_frame_cc(ctx, dup);

                        if ((ret = ff_filter_frame(outlink, dup)) < 0)
                            return ret;

                        s->frames_out++;
                        if (j > 0) s->dup++;
                    }
                    av_frame_free(&buf);
                } else {
                    /* for delta less or equal to 0, we should drop the frame,
                     * otherwise, we will have one or more extra frames */
                    av_frame_free(&buf);
                    s->drop++;
                }
            }
        }
        return 0;
    }

    return ret;
}

static int write_to_fifo(AVFifoBuffer *fifo, AVFrame *buf)
{
    int ret;

    if (!av_fifo_space(fifo) &&
        (ret = av_fifo_realloc2(fifo, 2*av_fifo_size(fifo)))) {
        av_frame_free(&buf);
        return ret;
    }

    av_fifo_generic_write(fifo, &buf, sizeof(buf), NULL);
    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *buf)
{
    AVFilterContext    *ctx = inlink->dst;
    FPSContext           *s = ctx->priv;
    AVFilterLink   *outlink = ctx->outputs[0];
    int64_t delta;
    int i, ret;

    /* Read the A53 side data, discard padding, and put 608/708 into
       queues so we can ensure they get into the output frames at
       the correct rate... */
    if (s->expected_cc_count > 0) {
        AVFrameSideData *side_data = av_frame_get_side_data(buf, AV_FRAME_DATA_A53_CC);
        if (side_data) {
            uint8_t *cc_bytes = side_data->data;
            int cc_count = side_data->size / CC_BYTES_PER_ENTRY;
            s->cc_detected = 1;

            for (i = 0; i < cc_count; i++) {
                /* See ANSI/CTA-708-E Sec 4.3, Table 3 */
                uint8_t cc_valid = (cc_bytes[CC_BYTES_PER_ENTRY*i] & 0x04) >> 2;
                uint8_t cc_type = cc_bytes[CC_BYTES_PER_ENTRY*i] & 0x03;
                if (cc_type == 0x00 || cc_type == 0x01) {
                    av_fifo_generic_write(s->cc_608_fifo, &cc_bytes[CC_BYTES_PER_ENTRY*i],
                                          CC_BYTES_PER_ENTRY, NULL);
                } else if (cc_valid && (cc_type == 0x02 || cc_type == 0x03)) {
                    av_fifo_generic_write(s->cc_708_fifo, &cc_bytes[CC_BYTES_PER_ENTRY*i],
                                          CC_BYTES_PER_ENTRY, NULL);
                }
            }
            /* Remove the side data, as we will re-create it on the
               output as needed */
            av_frame_remove_side_data(buf, AV_FRAME_DATA_A53_CC);
        }
    }

    s->frames_in++;
    /* discard frames until we get the first timestamp */
    if (s->first_pts == AV_NOPTS_VALUE) {
        if (buf->pts != AV_NOPTS_VALUE) {
            ret = write_to_fifo(s->fifo, buf);
            if (ret < 0)
                return ret;

            if (s->start_time != DBL_MAX && s->start_time != AV_NOPTS_VALUE) {
                double first_pts = s->start_time * AV_TIME_BASE;
                first_pts = FFMIN(FFMAX(first_pts, INT64_MIN), INT64_MAX);
                s->first_pts = av_rescale_q(first_pts, AV_TIME_BASE_Q,
                                                     inlink->time_base);
                av_log(ctx, AV_LOG_VERBOSE, "Set first pts to (in:%"PRId64" out:%"PRId64")\n",
                       s->first_pts, av_rescale_q(first_pts, AV_TIME_BASE_Q,
                                                  outlink->time_base));
            } else {
                s->first_pts = buf->pts;
            }
        } else {
            av_log(ctx, AV_LOG_WARNING, "Discarding initial frame(s) with no "
                   "timestamp.\n");
            av_frame_free(&buf);
            s->drop++;
        }
        return 0;
    }

    /* now wait for the next timestamp */
    if (buf->pts == AV_NOPTS_VALUE || av_fifo_size(s->fifo) <= 0) {
        return write_to_fifo(s->fifo, buf);
    }

    /* number of output frames */
    delta = av_rescale_q_rnd(buf->pts - s->first_pts, inlink->time_base,
                             outlink->time_base, s->rounding) - s->frames_out ;

    if (delta < 1) {
        /* drop everything buffered except the last */
        int drop = av_fifo_size(s->fifo)/sizeof(AVFrame*);

        av_log(ctx, AV_LOG_DEBUG, "Dropping %d frame(s).\n", drop);
        s->drop += drop;

        flush_fifo(s->fifo);
        ret = write_to_fifo(s->fifo, buf);

        return ret;
    }

    /* can output >= 1 frames */
    for (i = 0; i < delta; i++) {
        AVFrame *buf_out;
        av_fifo_generic_read(s->fifo, &buf_out, sizeof(buf_out), NULL);

        /* duplicate the frame if needed */
        if (!av_fifo_size(s->fifo) && i < delta - 1) {
            AVFrame *dup = av_frame_clone(buf_out);

            av_log(ctx, AV_LOG_DEBUG, "Duplicating frame.\n");
            if (dup)
                ret = write_to_fifo(s->fifo, dup);
            else
                ret = AVERROR(ENOMEM);

            if (ret < 0) {
                av_frame_free(&buf_out);
                av_frame_free(&buf);
                return ret;
            }

            s->dup++;
        }

        buf_out->pts = av_rescale_q(s->first_pts, inlink->time_base,
                                    outlink->time_base) + s->frames_out;
        handle_frame_cc(ctx, buf_out);

        if ((ret = ff_filter_frame(outlink, buf_out)) < 0) {
            av_frame_free(&buf);
            return ret;
        }

        s->frames_out++;
    }
    flush_fifo(s->fifo);

    ret = write_to_fifo(s->fifo, buf);

    return ret;
}

static const AVFilterPad avfilter_vf_fps_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_fps_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .request_frame = request_frame,
        .config_props  = config_props
    },
    { NULL }
};

AVFilter ff_vf_fps = {
    .name        = "fps",
    .description = NULL_IF_CONFIG_SMALL("Force constant framerate."),
    .init        = init,
    .uninit      = uninit,
    .priv_size   = sizeof(FPSContext),
    .priv_class  = &fps_class,
    .inputs      = avfilter_vf_fps_inputs,
    .outputs     = avfilter_vf_fps_outputs,
};
