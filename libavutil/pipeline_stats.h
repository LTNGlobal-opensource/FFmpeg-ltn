/*
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

#ifndef AVUTIL_PIPELINE_STATS_H
#define AVUTIL_PIPELINE_STATS_H

#include "frame.h"

typedef struct AVPipelineStats {
    int64_t input_pts;
    int64_t output_pts;

    int64_t avprotocol_arrival_time; /* Network arrival time - socket/file read() */
    int64_t avformat_input_time; /* Arrived at demux url_read(data/len) */
    int64_t avformat_read_time;  /* Leaving demux - av_read_frame(avpacket) */
    int64_t avcodec_decode_start; /* avcodec_send_packet(avpacket) */
    int64_t avcodec_decode_end; /* avcodec_receive_frame(avframe) */
    int64_t avfilter_graph_start; /* av_buffersrc_add_frame_flags(avframe) */
    int64_t avfilter_graph_end; /* avfilter_graph_request_oldest(avframe) */
    int64_t avcodec_encode_start; /* avcodec_send_frame(avframe) */
    int64_t avcodec_encode_end; /* avcodec_receive_packet(avpacket) */
    int64_t avformat_write_time; /* Entering mux - av_write(pkt) */
    int64_t avformat_mod_write_time; /* Write to actual avformat module */
    int64_t avformat_output_time; /* Leaving actual avformat module - socket/file write() */
} AVPipelineStats;

enum pipeline_stat {
    AVPROTOCOL_ARRIVAL_TIME,
    AVFORMAT_INPUT_TIME,
    AVFORMAT_READ_TIME,
    AVCODEC_DECODE_START,
    AVCODEC_DECODE_END,
    AVFILTER_GRAPH_START,
    AVFILTER_GRAPH_END,
    AVCODEC_ENCODE_START,
    AVCODEC_ENCODE_END,
    AVFORMAT_WRITE_TIME,
    AVFORMAT_MOD_WRITE_TIME,
    AVFORMAT_OUTPUT_TIME
};

void avframe_update_pipelinestats(struct AVFrame *frame, enum pipeline_stat stat, int64_t ts,
                                  int64_t in_pts, int64_t out_pts);
void avutil_update_pipelinestats(struct AVPipelineStats *stats, enum pipeline_stat stat, int64_t ts,
                                 int64_t in_pts, int64_t out_pts);

#endif /* AVUTIL_PIPELINE_STATS_H */
