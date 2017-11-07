/*
 * Copyright (c) 2015 The FFmpeg Project
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

#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "audio.h"
#include "formats.h"
#include <sys/time.h>

struct timeval avsyncmeasure_tv;

typedef struct AvSyncMeasureContext {
    const AVClass *class;
} AvSyncMeasureContext;

#define OFFSET(x) offsetof(AvSyncMeasureContext, x)
#define A AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption avsyncmeasure_options[] = {
    { NULL }
};

AVFILTER_DEFINE_CLASS(avsyncmeasure);

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    AVFilterChannelLayouts *layout = NULL;
    int ret;

    if ((ret = ff_add_format                 (&formats, AV_SAMPLE_FMT_FLT  )) < 0 ||
        (ret = ff_set_common_formats         (ctx     , formats            )) < 0 ||
        (ret = ff_add_channel_layout         (&layout , AV_CH_LAYOUT_STEREO)) < 0 ||
        (ret = ff_set_common_channel_layouts (ctx     , layout             )) < 0)
        return ret;

    formats = ff_all_samplerates();
    return ff_set_common_samplerates(ctx, formats);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    AvSyncMeasureContext *s = ctx->priv;
    const float *src = (const float *)in->data[0];
    AVFrame *out;
    float *dst;
    int n;

    if (av_frame_is_writable(in)) {
        out = in;
    } else {
        out = ff_get_audio_buffer(inlink, in->nb_samples);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, in);
    }
    dst = (float *)out->data[0];



//    printf("%ld.%06d %f %f %f %f\n", avsyncmeasure_tv.tv_sec, avsyncmeasure_tv.tv_usec, src[0], src[1], src[2], src[3]);
    if (src[0] != 0.0 || src[1] != 0.0 || src[2] != 0.0 || src[3] != 0.0) {
//        printf("%ld.%06d %f %f %f %f\n", avsyncmeasure_tv.tv_sec, avsyncmeasure_tv.tv_usec, src[0], src[1], src[2], src[3]);
        gettimeofday(&avsyncmeasure_tv, NULL);
        printf("%ld.%06d audio\n", avsyncmeasure_tv.tv_sec, avsyncmeasure_tv.tv_usec);
    }

    for (n = 0; n < in->nb_samples; n++) {
        dst[n] = src[n];
    }

    if (out != in)
        av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_AUDIO,
    },
    { NULL }
};

AVFilter ff_af_avsyncmeasure = {
    .name           = "avsyncmeasure",
    .description    = NULL_IF_CONFIG_SMALL("Look for pip in audio"),
    .query_formats  = query_formats,
    .priv_size      = sizeof(AvSyncMeasureContext),
    .priv_class     = &avsyncmeasure_class,
    .inputs         = inputs,
    .outputs        = outputs,
};
