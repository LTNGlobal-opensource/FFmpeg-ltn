/*
 * A53 Closed Caption Deletion Filter
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
 * A53 Closed Caption Side-data Deletion Filter.  As opposed to the vf_sidedata
 * filter, this filter allows for the removal of just the CEA-608 or CEA-708
 * captions (replacing it with A53 padding).
 */

#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "internal.h"

typedef struct A53Context {
    const AVClass *class;
    int delete_708;
    int delete_608;
} A53Context;

static int filter_frame(AVFilterLink *link, AVFrame *frame)
{
    A53Context *s = link->dst->priv;
    AVFrameSideData *side_data;
    uint8_t *cc_bytes;
    int i;
    int cc_count;

    side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_A53_CC);
    if (!side_data) {
        /* No A53 side data present, so just pass frame through */
        return ff_filter_frame(link->dst->outputs[0], frame);
    }

    cc_bytes = side_data->data;
    cc_count = side_data->size / 3;

    for (i = 0; i < cc_count; i++) {
        uint8_t cc_type = cc_bytes[3*i] & 0x03;
        if (((cc_type == 0x00 || cc_type == 0x01) && s->delete_608) ||
            ((cc_type == 0x02 || cc_type == 0x03) && s->delete_708))
        {
            /* Clear CC valid and blank out cc_data1, cc_data2 */
            cc_bytes[3*i]   = 0xfa;
            cc_bytes[3*i+1] = 0x00;
            cc_bytes[3*i+2] = 0x00;
        }
    }

    return ff_filter_frame(link->dst->outputs[0], frame);
}

#define OFFSET(x) offsetof(A53Context, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption a53_options[] = {
    { "delete608","Delete 608 caption data if found", OFFSET(delete_608), AV_OPT_TYPE_BOOL, { .i64 = 0}, 0, 1, .flags = FLAGS },
    { "delete708","Delete 708 caption data if found", OFFSET(delete_708), AV_OPT_TYPE_BOOL, { .i64 = 0}, 0, 1, .flags = FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(a53);

static const AVFilterPad avfilter_vf_a53_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_a53_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_a53 = {
    .name        = "a53",
    .description = NULL_IF_CONFIG_SMALL("Remove 608 or 708 captions from video frames"),
    .priv_size   = sizeof(A53Context),
    .priv_class  = &a53_class,
    .inputs      = avfilter_vf_a53_inputs,
    .outputs     = avfilter_vf_a53_outputs,
};
