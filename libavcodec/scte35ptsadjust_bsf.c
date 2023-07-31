/*
 * SCTE-35 PTS fixup bitstream filter
 * Copyright (c) 2023 LTN Global Communications, Inc.
 *
 * Author: Devin Heitmueller <dheitmueller@ltnglobal.com>
 *
 * Because SCTE-35 messages are represented in TS streams as sections
 * rather than PES packets, we cannot rely on ffmpeg's standard
 * mechanisms to adjust PTS values if reclocking the stream.
 * This filter will leverage the SCTE-35 pts_adjust field to
 * compensate for any change in the PTS values in the stream.
 *
 * See SCTE-35 2019 Sec 9.6 for information about the use of
 * the pts_adjust field.
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

#include "bsf.h"
#include "bsf_internal.h"
#include "defs.h"
#include "libavutil/intreadwrite.h"

static int scte35ptsadjust_filter(AVBSFContext *ctx, AVPacket *pkt)
{
    const AVTransportTimestamp *transport_ts;
    int64_t cur_pts_adjust;
    int ret = 0;

    ret = ff_bsf_get_packet_ref(ctx, pkt);
    if (ret < 0)
        return ret;

    /* Retrieve the original PTS, which will be used to calculate the pts_adjust */
    transport_ts = (AVTransportTimestamp *) av_packet_get_side_data(pkt, AV_PKT_DATA_TRANSPORT_TIMESTAMP, NULL);
    if (transport_ts == NULL) {
        /* No original PTS specified, so just pass the packet through */
        return 0;
    }

    /* The pts_adjust field is logically buf[4]-buf[8] of the payload */
    if (pkt->size < 9) {
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    /* Extract the current pts_adjust value from the packet */
    cur_pts_adjust = ((int64_t)(pkt->data[4] & 1) << 32 ) |
	    AV_RB32(pkt->data + 5);

    av_log(ctx, AV_LOG_DEBUG, "pts=%" PRId64 "(%d/%d) orig_pts=%" PRId64 "(%d/%d) pts_adjust=%" PRId64 "\n",
           pkt->pts, pkt->time_base.num, pkt->time_base.den,
           transport_ts->pts, transport_ts->time_base.num, transport_ts->time_base.den, cur_pts_adjust);

    /* Compute the new PTS adjust value */
    cur_pts_adjust -= av_rescale_q(transport_ts->pts, transport_ts->time_base, (AVRational){1, 90000});
    cur_pts_adjust += av_rescale_q(pkt->pts, pkt->time_base, (AVRational){1, 90000});
    cur_pts_adjust &= 0x1FFFFFFFFLL;

    av_log(ctx, AV_LOG_DEBUG, "new pts_adjust=%" PRId64 "\n", cur_pts_adjust);

    ret = av_packet_make_writable(pkt);
    if (ret < 0)
        goto fail;

    /* Insert the updated pts_adjust value */
    pkt->data[4] &= 0xfe; /* Preserve top 7 unrelated bits */
    pkt->data[4] |= cur_pts_adjust >> 32;
    AV_WB32(pkt->data + 5, cur_pts_adjust);

fail:
    if (ret < 0)
        av_packet_unref(pkt);

    return ret;
}

static const enum AVCodecID codec_ids[] = {
    AV_CODEC_ID_SCTE_35, AV_CODEC_ID_NONE,
};

const FFBitStreamFilter ff_scte35ptsadjust_bsf = {
    .p.name         = "scte35ptsadjust",
    .p.codec_ids    = codec_ids,
    .filter         = scte35ptsadjust_filter,
};
