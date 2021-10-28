/*
 * Copyright (c) 2020 LTN Global Communications Inc.
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

#include "config.h"

#define _GNU_SOURCE
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "thread.h"
#include "common.h"
#include "pipeline_stats.h"

void avframe_update_pipelinestats(struct AVFrame *frame, enum pipeline_stat stat, int64_t ts,
                                  int64_t in_pts, int64_t out_pts)
{
    AVFrameSideData *side_data;

    /* See if there is already side data.  If so, update it.  If not, create it */
    side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_PIPELINE_STATS);
    if (side_data == NULL) {
        side_data = av_frame_new_side_data(frame, AV_FRAME_DATA_PIPELINE_STATS,
                                           sizeof(struct AVPipelineStats));
        if (side_data && side_data->data)
            memset(side_data->data, 0, sizeof(struct AVPipelineStats));
    }
    if (side_data)
        avutil_update_pipelinestats((struct AVPipelineStats *) side_data->data,
                                    stat, ts, in_pts, out_pts);
}

void avutil_update_pipelinestats(struct AVPipelineStats *stats, enum pipeline_stat stat, int64_t ts,
                                 int64_t in_pts, int64_t out_pts)
{
    if (stats == NULL)
        return;

    if (in_pts != -1)
        stats->input_pts = in_pts;
    if (out_pts != -1)
        stats->output_pts = out_pts;

    switch (stat) {
    case AVPROTOCOL_ARRIVAL_TIME:
        stats->avprotocol_arrival_time = ts;
        break;
    case AVFORMAT_INPUT_TIME:
        stats->avformat_input_time = ts;
        break;
    case AVFORMAT_READ_TIME:
        stats->avformat_read_time = ts;
        break;
    case AVCODEC_DECODE_START:
        stats->avcodec_decode_start = ts;
        break;
    case AVCODEC_DECODE_END:
        stats->avcodec_decode_end = ts;
        break;
    case AVFILTER_GRAPH_START:
        stats->avfilter_graph_start = ts;
        break;
    case AVFILTER_GRAPH_END:
        stats->avfilter_graph_end = ts;
        break;
    case AVCODEC_ENCODE_START:
        stats->avcodec_encode_start = ts;
        break;
    case AVCODEC_ENCODE_END:
        stats->avcodec_encode_end = ts;
        break;
    case AVFORMAT_WRITE_TIME:
        stats->avformat_write_time = ts;
        break;
    case AVFORMAT_MOD_WRITE_TIME:
        stats->avformat_mod_write_time = ts;
        break;
    case AVFORMAT_OUTPUT_TIME:
        stats->avformat_output_time = ts;
        break;
    }
#if 0
    if (stat == AVFORMAT_OUTPUT_TIME) {
        fprintf(stderr, "stat=%d in_pts=%"PRId64" a=%"PRId64" i=%"PRId64" r=%"PRId64" d=%"PRId64" e=%"PRId64" gs=%"PRId64" ge=%"PRId64" es=%"PRId64" ee=%"PRId64" wt=%"PRId64" o=%"PRId64"\n",
               stat, stats->input_pts, stats->avprotocol_arrival_time, stats->avformat_input_time, stats->avformat_read_time,
               stats->avcodec_decode_start, stats->avcodec_decode_end, stats->avfilter_graph_start,
               stats->avfilter_graph_end, stats->avcodec_encode_start, stats->avcodec_encode_end,
               stats->avformat_write_time, stats->avformat_output_time
            );
    }
#endif
}
