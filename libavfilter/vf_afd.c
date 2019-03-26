/*
 * AFD and Bardata Insertion Filter
 * Copyright (c) 2018 LTN Global Communications, Inc.
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
 * Active Format Description and Bar Data Insertion Filter
 */

#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "internal.h"

typedef struct AFDContext {
    const AVClass *class;
    int enable_afd;
    int afd_code;
    int afd_cycle;
    int enable_bardata;
    int top;
    int bottom;
    int left;
    int right;
    int fcount;
} AFDContext;

static int filter_frame(AVFilterLink *link, AVFrame *frame)
{
    AFDContext *s = link->dst->priv;
    AVFrameSideData *side_data;
    AVBarData *bar_data;

    /* Support incremeting the AFD every 10 seconds.  This is really
       just a feature for creating test streams... */
    if (s->afd_cycle && s->fcount++ % 600 == 0) {
        if (s->afd_code == 15)
            s->afd_code = 0;
        else
            s->afd_code++;
    }

    /* Insert/tweak the side-data for AFD */
    if (s->enable_afd) {
        /* Insert/tweak the side-data for Bar Data */
        side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_AFD);
        if (!side_data) {
            side_data = av_frame_new_side_data(frame, AV_FRAME_DATA_AFD, sizeof(unsigned char));
            if (side_data == NULL)
                return -ENOMEM;
        }
        side_data->data[0] = s->afd_code;
    }

    if (s->enable_bardata) {
        /* Insert/tweak the side-data for Bar Data */
        side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_BARDATA);
        if (!side_data) {
            side_data = av_frame_new_side_data(frame, AV_FRAME_DATA_BARDATA, sizeof(AVBarData));
            if (side_data == NULL)
                return -ENOMEM;
        }
        bar_data = (AVBarData *) side_data->data;
        if (s->top || s->bottom) {
            bar_data->top_bottom = 1;
            bar_data->top = s->top;
            bar_data->bottom = s->bottom;
            bar_data->left = 0;
            bar_data->right = 0;
        } else {
            bar_data->top_bottom = 0;
            bar_data->top = 0;
            bar_data->bottom = 0;
            bar_data->left = s->left;
            bar_data->right = s->right;
        }
    }

    return ff_filter_frame(link->dst->outputs[0], frame);
}

#define OFFSET(x) offsetof(AFDContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption setafd_options[] = {
    /* AFD Options */
    { "afd",    "Enable AFD insertion", OFFSET(enable_afd), AV_OPT_TYPE_BOOL, { .i64 = 0}, 0, 1, .flags = FLAGS },
    { "code",   "AFD code to insert", OFFSET(afd_code), AV_OPT_TYPE_INT, {.i64=0}, 0, 0x0F, FLAGS },
    { "cycle",  "Cycle through AFD codes for testing/debug", OFFSET(afd_cycle), AV_OPT_TYPE_BOOL, { .i64 = 0}, 0, 1, .flags = FLAGS },

    /* Bar data Options */
    { "bardata","Enable Bar Data insertion", OFFSET(enable_bardata), AV_OPT_TYPE_BOOL, { .i64 = 0}, 0, 1, .flags = FLAGS },
    { "top",   "top bar position", OFFSET(top), AV_OPT_TYPE_INT, {.i64=0}, 0, INT_MAX, FLAGS },
    { "bottom","bottom bar position", OFFSET(bottom), AV_OPT_TYPE_INT, {.i64=0}, 0, INT_MAX, FLAGS },
    { "left",  "left bar position", OFFSET(left), AV_OPT_TYPE_INT, {.i64=0}, 0, INT_MAX, FLAGS },
    { "right", "right bar position", OFFSET(right), AV_OPT_TYPE_INT, {.i64=0}, 0, INT_MAX, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(setafd);

static const AVFilterPad avfilter_vf_setafd_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_setafd_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_setafd = {
    .name        = "setafd",
    .description = NULL_IF_CONFIG_SMALL("Set AFD and/or Bar Data for video frames"),
    .priv_size   = sizeof(AFDContext),
    .priv_class  = &setafd_class,
    .inputs      = avfilter_vf_setafd_inputs,
    .outputs     = avfilter_vf_setafd_outputs,
};
