/*
 * SCTE-35 PTS fixup bitstream filter
 * Copyright (c) 2020 LTN Global Communications, Inc.
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

#include "avcodec.h"
#include "bsf.h"
#include "bsf_internal.h"

static int scte35ptsadjust_init(AVBSFContext *ctx)
{
    return 0;
}

static int scte35ptsadjust_filter(AVBSFContext *ctx, AVPacket *out)
{
    AVPacket *in;
    int size;
    const int64_t *orig_pts;
    int64_t cur_pts_adjust;
    int ret = 0;

    ret = ff_bsf_get_packet(ctx, &in);
    if (ret < 0)
        return ret;

    /* Retrieve the original PTS, which will be used to calculate the pts_adjust */
    orig_pts = (int64_t *) av_packet_get_side_data(in, AV_PKT_DATA_ORIG_PTS, &size);
    av_log(ctx, AV_LOG_DEBUG, "%s pts=%" PRId64 " orig_pts=%" PRId64 "\n", __func__,
           in->pts, orig_pts ? *orig_pts : 0);

    /* The pts_adjust field is logically buf[4]-buf[8] of the payload */
    if (in->size < 8)
        goto fail;

    /* Extract the current pts_adjust value from the packet */
    cur_pts_adjust = ((int64_t) in->data[4] & (int64_t) 0x01 << 32) |
                     ((int64_t) in->data[5] << 24) |
                     ((int64_t) in->data[6] << 16) |
                     ((int64_t) in->data[7] << 8) |
                     ((int64_t) in->data[8] << 8);

    av_log(ctx, AV_LOG_DEBUG, "%s pts_adjust=%" PRId64 "\n", __func__,
           cur_pts_adjust);

    /* Compute the new PTS adjust value */
    cur_pts_adjust -= *orig_pts;
    cur_pts_adjust += in->pts;
    cur_pts_adjust &= (int64_t) 0x1ffffffff;

    av_log(ctx, AV_LOG_DEBUG, "%s new pts_adjust=%" PRId64 "\n", __func__,
           cur_pts_adjust);

    /* Clone the incoming packet since we need to modify it */
    ret = av_new_packet(out, in->size);
    if (ret < 0)
        goto fail;
    ret = av_packet_copy_props(out, in);
    if (ret < 0)
        goto fail;
    memcpy(out->data, in->data, in->size);

    /* Insert the updated pts_adjust value */
    out->data[4] &= 0xfe; /* Preserve top 7 unrelated bits */
    out->data[4] |= cur_pts_adjust >> 32;
    out->data[5] = cur_pts_adjust >> 24;
    out->data[6] = cur_pts_adjust >> 16;
    out->data[7] = cur_pts_adjust >> 8;
    out->data[8] = cur_pts_adjust;

fail:
    av_packet_free(&in);

    return ret;
}

static const enum AVCodecID codec_ids[] = {
    AV_CODEC_ID_SCTE_35, AV_CODEC_ID_NONE,
};

const AVBitStreamFilter ff_scte35ptsadjust_bsf = {
    .name           = "scte35ptsadjust",
    .init           = scte35ptsadjust_init,
    .filter         = scte35ptsadjust_filter,
    .codec_ids      = codec_ids,
};
