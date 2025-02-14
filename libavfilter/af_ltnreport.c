/*
 * Copyright (c) 2025 LTN Global Communications Inc.
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

#include "avfilter.h"
#include "filters.h"
#include "audio.h"
#include "libavformat/ltnlog.h"

typedef struct ltnreport_context {
    const AVClass *class;
    int64_t last_reported;
} ltnreport_context;

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    ltnreport_context *s = ctx->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    AVFrame *out = ff_get_audio_buffer(outlink, in->nb_samples);
    AVRational rebase = av_mul_q(in->time_base, av_make_q(10, 1));
    int64_t target_time = s->last_reported + (rebase.den / rebase.num); /* Every 1/10 second */
    const char *filtersource = "unknown";
    AVDictionaryEntry *e;
    int ret;

    if (!out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    e = av_dict_get(in->metadata, "filtersource", NULL, 0);
    if (e && e->value) {
        filtersource = e->value;
    }

    if (in->pts > target_time) {
        /* Check for up to 16 channels of audio on this stream */
        for (int i = 0; i < 16; i++) {
            char key[256];

            snprintf(key, sizeof(key), "lavfi.astats.%d.RMS_level", i+1);
            e = av_dict_get(in->metadata, key, NULL, 0);
            if (e && e->value) {
                ltnlog_msg("AUDIOLEVEL", "%s,%d,%s", filtersource, i+1, e->value);
            }
        }
        s->last_reported = in->pts;
    }

    ret = av_frame_copy_props(out, in);
    if (ret < 0)
        goto fail;
    ret = av_frame_copy(out, in);
    if (ret < 0)
        goto fail;
    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
fail:
    av_frame_free(&in);
    av_frame_free(&out);
    return ret;
}

static const AVFilterPad ltnreport_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = filter_frame,
    },
};

const AVFilter ff_af_ltnreport = {
    .name          = "ltnreport",
    .description   = NULL_IF_CONFIG_SMALL("Report audio stats back to LTN controller"),
    .priv_size     = sizeof(ltnreport_context),
    .flags         = AVFILTER_FLAG_METADATA_ONLY,
    FILTER_INPUTS(ltnreport_inputs),
    FILTER_OUTPUTS(ff_audio_default_filterpad),
};
