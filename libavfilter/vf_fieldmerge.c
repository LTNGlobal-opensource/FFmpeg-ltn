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

#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "libavutil/avassert.h"

#include "formats.h"
#include "avfilter.h"
#include "interlace.h"
#include "internal.h"
#include "video.h"

#define OFFSET(x) offsetof(InterlaceContext, x)

static const AVOption interlace_options[] = {
    { NULL }
};

AVFILTER_DEFINE_CLASS(interlace);

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

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *fmts_list = ff_make_format_list(formats_supported);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    InterlaceContext *s = ctx->priv;

    av_frame_free(&s->cur);
    av_frame_free(&s->next);
}

static int supported_format(AVFilterLink *inlink)
{
    if (inlink->interlaced_frame == 0)
        return 0;
    if (inlink->w == 1920 && inlink->h == 540)
        return 1;
    if (inlink->w == 720 && inlink->h == 240)
        return 1;
    return 0;
}

static int config_out_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = outlink->src->inputs[0];
    InterlaceContext *s = ctx->priv;

    if (inlink->h < 2) {
        av_log(ctx, AV_LOG_ERROR, "input video height is too small\n");
        return AVERROR_INVALIDDATA;
    }

    if (supported_format(inlink)) {
        outlink->w = inlink->w;
        outlink->h = inlink->h * 2;
        outlink->time_base = inlink->time_base;
        outlink->frame_rate = inlink->frame_rate;

        /* half framerate */
        outlink->time_base.num *= 2;
        outlink->frame_rate.den *= 2;

        s->csp = av_pix_fmt_desc_get(outlink->format);
    } else {
        /* Just pass it through */
        outlink->w = inlink->w;
        outlink->h = inlink->h;
        outlink->time_base = inlink->time_base;
        outlink->frame_rate = inlink->frame_rate;
        return 0;
    }

    return 0;
}

static void copy_picture_field(InterlaceContext *s,
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
    InterlaceContext *s = ctx->priv;
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

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out)
        return AVERROR(ENOMEM);

    av_frame_copy_props(out, s->cur);
    out->interlaced_frame = 1;
    out->top_field_first  = s->cur->top_field_first;
    out->pts             /= 2;  // adjust pts to new framerate

    /* AVFrame doesn't support just sending an individual field, so as a
       hack let's look for some metadata associated with the AVFrame to
       tell us whether it's the top field or bottom (and hevcdec.c sets
       the metadata upstream) */
    AVDictionaryEntry *entry;
    enum AVPictureStructure pic_struct_cur = AV_PICTURE_STRUCTURE_UNKNOWN;
    enum AVPictureStructure pic_struct_next = AV_PICTURE_STRUCTURE_UNKNOWN;
    entry = av_dict_get(s->cur->metadata, "pic_struct", NULL, 0);
    if (entry != NULL) {
        pic_struct_cur = atoi(entry->value);
    }
    entry = av_dict_get(s->next->metadata, "pic_struct", NULL, 0);
    if (entry != NULL) {
        pic_struct_next = atoi(entry->value);
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
    },
    { NULL }
};

static const AVFilterPad outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_out_props,
    },
    { NULL }
};

AVFilter ff_vf_fieldmerge = {
    .name          = "fieldmerge",
    .description   = NULL_IF_CONFIG_SMALL("Convert frames containing fields into real interlaced frames"),
    .uninit        = uninit,
    .priv_class    = &interlace_class,
    .priv_size     = sizeof(InterlaceContext),
    .query_formats = query_formats,
    .inputs        = inputs,
    .outputs       = outputs,
};
