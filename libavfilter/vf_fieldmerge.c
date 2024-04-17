/*
 * vf_fieldmerge: Convert fields into frames
 * Copyright (c) 2018 LTN Global Communications, Inc.
 *
 * This is a nasty, terrible hack to deal with the HEVC decoder
 * not currently supporting picture structures composed of fields.
 * We take AVFrames which really contain fields (i.e. 1920x540), and
 * combine them into real frames again.  We rely on a custom AVFrame
 * metadata field to know if the AVFrame contains a top field or bottom
 * (which we've hacked hevcdec.c to insert).
 *
 * Derived from vf_interlace.c, with the following copyrights:
 * Copyright (c) 2003 Michael Zucchi <notzed@ximian.com>
 * Copyright (c) 2010 Baptiste Coudurier
 * Copyright (c) 2011 Stefano Sabatini
 * Copyright (c) 2013 Vittorio Giovara <vittorio.giovara@gmail.com>
 * Copyright (c) 2017 Thomas Mundt <tmundt75@gmail.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @file
 * progressive to interlaced content filter, inspired by heavy debugging of tinterlace filter
 */


#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "libavutil/avassert.h"
#include "libavcodec/avcodec.h"
#include "avfilter.h"
#include "filters.h"
#include "video.h"

typedef struct FieldmergeContext {
    const AVClass *class;
    AVFrame *cur;
    AVFrame *next;
    const AVPixFmtDescriptor *csp;
} FieldmergeContext;

#define OFFSET(x) offsetof(FieldmergeContext, x)

static const AVOption fieldmerge_options[] = {
    { NULL }
};

AVFILTER_DEFINE_CLASS(fieldmerge);

static const enum AVPixelFormat formats_supported[] = {
    AV_PIX_FMT_YUV410P,      AV_PIX_FMT_YUV411P,
    AV_PIX_FMT_YUV420P,      AV_PIX_FMT_YUV422P,      AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUV420P10LE,  AV_PIX_FMT_YUV422P10LE,  AV_PIX_FMT_YUV444P10LE,
    AV_PIX_FMT_YUV420P12LE,  AV_PIX_FMT_YUV422P12LE,  AV_PIX_FMT_YUV444P12LE,
    AV_PIX_FMT_YUVA420P,     AV_PIX_FMT_YUVA422P,     AV_PIX_FMT_YUVA444P,
    AV_PIX_FMT_YUVA420P10LE, AV_PIX_FMT_YUVA422P10LE, AV_PIX_FMT_YUVA444P10LE,
    AV_PIX_FMT_GRAY8,        AV_PIX_FMT_YUVJ420P,     AV_PIX_FMT_YUVJ422P,
    AV_PIX_FMT_YUVJ444P,     AV_PIX_FMT_YUVJ440P,     AV_PIX_FMT_NONE
};

static av_cold void uninit(AVFilterContext *ctx)
{
    FieldmergeContext *s = ctx->priv;

    av_frame_free(&s->cur);
    av_frame_free(&s->next);
}

static int supported_format(AVFilterLink *inlink)
{
    if (inlink->w == 1920 && inlink->h == 540)
        return 1;
    if (inlink->w == 720 && inlink->h == 240)
        return 1;
    if (inlink->w == 720 && inlink->h == 288)
        return 1;
    return 0;
}

static int config_out_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = outlink->src->inputs[0];
    FieldmergeContext *s = ctx->priv;
    FilterLink *il = ff_filter_link(inlink);
    FilterLink *l = ff_filter_link(outlink);

    if (inlink->h < 2) {
        av_log(ctx, AV_LOG_ERROR, "input video height is too small\n");
        return AVERROR_INVALIDDATA;
    }

    if (supported_format(inlink)) {
        outlink->w = inlink->w;
        outlink->h = inlink->h * 2;
        outlink->time_base = inlink->time_base;
        l->frame_rate = il->frame_rate;

        /* half framerate */
        outlink->time_base.num *= 2;
        l->frame_rate.den *= 2;

        s->csp = av_pix_fmt_desc_get(outlink->format);
    } else {
        /* Just pass it through */
        outlink->w = inlink->w;
        outlink->h = inlink->h;
        outlink->time_base = inlink->time_base;
        l->frame_rate = il->frame_rate;
        return 0;
    }

    return 0;
}

static void copy_picture_field(FieldmergeContext *s,
                               AVFrame *src_frame, AVFrame *dst_frame,
                               AVFilterLink *inlink,
                               enum AVPictureStructure pic_struct)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    int hsub = desc->log2_chroma_w;
    int vsub = desc->log2_chroma_h;
    int plane;

    for (plane = 0; plane < desc->nb_components; plane++) {
        int cols  = (plane == 1 || plane == 2) ? -(-inlink->w) >> hsub : inlink->w;
        int lines = (plane == 1 || plane == 2) ? AV_CEIL_RSHIFT(inlink->h, vsub) : inlink->h;
        uint8_t *dstp = dst_frame->data[plane];
        const uint8_t *srcp = src_frame->data[plane];
        int srcp_linesize = src_frame->linesize[plane];
        int dstp_linesize = dst_frame->linesize[plane] * 2;

        av_assert0(cols >= 0 || lines >= 0);

        if (pic_struct == AV_PICTURE_STRUCTURE_BOTTOM_FIELD) {
            dstp += dst_frame->linesize[plane];
        }

        if (s->csp->comp[plane].depth > 8)
            cols *= 2;
        av_image_copy_plane(dstp, dstp_linesize, srcp, srcp_linesize, cols, lines);
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *buf)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    FieldmergeContext *s = ctx->priv;
    AVDictionaryEntry *entry;
    enum AVPictureStructure pic_struct_cur = AV_PICTURE_STRUCTURE_UNKNOWN;
    enum AVPictureStructure pic_struct_next = AV_PICTURE_STRUCTURE_UNKNOWN;
    AVFrameSideData *f1_side_data = NULL;
    AVFrameSideData *f2_side_data = NULL;
    AVFrameSideData *out_side_data = NULL;
    AVFrame *out;
    int ret;

    if (inlink->h == outlink->h) {
        /* Bypassed, just pass through */
        return ff_filter_frame(outlink, buf);
    }

    av_frame_free(&s->cur);
    s->cur  = s->next;
    s->next = buf;

    /* we need at least two frames */
    if (!s->cur || !s->next)
        return 0;

    /* AVFrame doesn't support just sending an individual field, so as a
       hack let's look for some metadata associated with the AVFrame to
       tell us whether it's the top field or bottom (and hevcdec.c sets
       the metadata upstream) */
    entry = av_dict_get(s->cur->metadata, "pic_struct", NULL, 0);
    if (entry != NULL) {
        pic_struct_cur = atoi(entry->value);
    }
    entry = av_dict_get(s->next->metadata, "pic_struct", NULL, 0);
    if (entry != NULL) {
        pic_struct_next = atoi(entry->value);
    }

    if (pic_struct_cur == AV_PICTURE_STRUCTURE_BOTTOM_FIELD) {
        /* Throw away first BFF field so we re-align frames as TFF */
       return 0;
    }

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out)
        return AVERROR(ENOMEM);

    av_frame_copy_props(out, s->cur);
    out->flags |= AV_FRAME_FLAG_INTERLACED;
    if (s->cur->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST)
        out->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
    out->pts             /= 2;  /* adjust pts to new framerate */

    /* Combine any captions from the two fields */
    f1_side_data = av_frame_get_side_data(s->cur, AV_FRAME_DATA_A53_CC);
    f2_side_data = av_frame_get_side_data(s->next, AV_FRAME_DATA_A53_CC);

    if (f1_side_data && f2_side_data) {
        out_side_data = av_frame_new_side_data(out, AV_FRAME_DATA_A53_CC,
                                               f1_side_data->size + f2_side_data->size);
        if (out_side_data) {
            memcpy(&out_side_data->data[0], f1_side_data->data, f1_side_data->size);
            memcpy(&out_side_data->data[f1_side_data->size], f2_side_data->data,
                   f2_side_data->size);
        }
    } else if (f1_side_data) {
        out_side_data = av_frame_new_side_data(out, AV_FRAME_DATA_A53_CC,
                                               f1_side_data->size);
        if (out_side_data) {
            memcpy(&out_side_data->data[0], f1_side_data->data, f1_side_data->size);
        }
    } else if (f2_side_data) {
        out_side_data = av_frame_new_side_data(out, AV_FRAME_DATA_A53_CC,
                                               f2_side_data->size);
        if (out_side_data) {
            memcpy(&out_side_data->data[0], f2_side_data->data, f2_side_data->size);
        }
    }

    copy_picture_field(s, s->cur, out, inlink, pic_struct_cur);
    av_frame_free(&s->cur);

    copy_picture_field(s, s->next, out, inlink, pic_struct_next);
    av_frame_free(&s->next);

    ret = ff_filter_frame(outlink, out);

    return ret;
}

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    }
};

static const AVFilterPad outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_out_props,
    }
};

const AVFilter ff_vf_fieldmerge = {
    .name          = "fieldmerge",
    .description   = NULL_IF_CONFIG_SMALL("Convert frames containing fields into real interlaced frames"),
    .priv_size     = sizeof(FieldmergeContext),
    .uninit        = uninit,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_PIXFMTS_ARRAY(formats_supported),
    .priv_class    = &fieldmerge_class
};
