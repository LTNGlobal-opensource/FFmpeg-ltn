/*
 * vf_fieldextract: Quick alternative to deinterlacing
 * Copyright (c) 2018 LTN Global Communications, Inc.
 *
 * This module is a quick routine to go from interlaced to
 * progressive by field dropping, for cases where we're not
 * as concerned about quality.  In particular it's useful for
 * thumbnailing applications where the output resolution is
 * very low.  Example: 1920x1080 gets converted to 1920x540
 * with no interlacing artifacts.
 *
 * Note :The pixel aspect ratio  gets doubled on the output,
 * so while the aspect ratio is correct it might cause unexpected
 * behavior with operation such as overlay insertion that might
 * expect square pixels.
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

    outlink->w = inlink->w;
    outlink->h = inlink->h / 2;
    outlink->time_base = inlink->time_base;
    outlink->frame_rate = inlink->frame_rate;
    outlink->interlaced_frame = 0;
    outlink->top_field_first = 0;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    outlink->sample_aspect_ratio.den *= 2;

    s->csp = av_pix_fmt_desc_get(outlink->format);

    av_log(ctx, AV_LOG_VERBOSE, "Fieldextract w:%d h:%d interlace: %d tff:%d\n",
           outlink->w, outlink->h, inlink->interlaced_frame,
           inlink->top_field_first);

    return 0;
}

static void copy_picture_field(InterlaceContext *s,
                               AVFrame *src_frame, AVFrame *dst_frame,
                               AVFilterLink *inlink)
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
        int srcp_linesize = src_frame->linesize[plane] * 2;
        int dstp_linesize = dst_frame->linesize[plane];

        av_assert0(cols >= 0 || lines >= 0);

        lines /= 2;

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

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out)
        return AVERROR(ENOMEM);

    av_frame_copy_props(out, buf);
    out->interlaced_frame = 0;
    out->top_field_first  = 0;
    out->sample_aspect_ratio = buf->sample_aspect_ratio;
    out->sample_aspect_ratio.den *= 2;

    copy_picture_field(s, buf, out, inlink);
    av_frame_free(&buf);

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

AVFilter ff_vf_fieldextract = {
    .name          = "fieldextract",
    .description   = NULL_IF_CONFIG_SMALL("Extract the top field from interlaced frames"),
    .uninit        = uninit,
    .priv_class    = &interlace_class,
    .priv_size     = sizeof(InterlaceContext),
    .query_formats = query_formats,
    .inputs        = inputs,
    .outputs       = outputs,
};
