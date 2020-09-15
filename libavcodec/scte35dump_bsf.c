/*
 * SCTE-35 dump bitstream filter
 * Copyright (c) 2020 LTN Global Communications, Inc.
 *
 * This filter dumps out the contents of SCTE-35 messages
 * to the console, which is useful for debugging.
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

#include "libklscte35/scte35.h"

static int scte35dump_init(AVBSFContext *ctx)
{
    return 0;
}

static int scte35dump_filter(AVBSFContext *ctx, AVPacket *out)
{
    AVPacket *in;
    struct scte35_splice_info_section_s *s;
    int size;
    const int64_t *orig_pts;
    int ret = 0;

    ret = ff_bsf_get_packet(ctx, &in);
    if (ret < 0)
        return ret;

    /* Retrieve the original PTS, which will be used to calculate the pre-roll */
    orig_pts = (int64_t *) av_packet_get_side_data(in, AV_PKT_DATA_ORIG_PTS, &size);
    av_log(ctx, AV_LOG_INFO, "%s pts=%" PRId64 " orig_pts=%" PRId64 "\n", __func__,
           in->pts, orig_pts ? *orig_pts : 0);

    /* Parse the SCTE-35 packet */
    s = scte35_splice_info_section_parse(in->data, in->size);
    if (s == NULL) {
        av_log(ctx, AV_LOG_ERROR, "Failed to splice section.");
        return -1;
    }

    scte35_splice_info_section_print(s);

    scte35_splice_info_section_free(s);

    /* Pass through the packet */
    av_packet_move_ref(out, in);
    av_packet_free(&in);

    return ret;
}

static const enum AVCodecID codec_ids[] = {
    AV_CODEC_ID_SCTE_35, AV_CODEC_ID_NONE,
};

const AVBitStreamFilter ff_scte35dump_bsf = {
    .name           = "scte35dump",
    .init           = scte35dump_init,
    .filter         = scte35dump_filter,
    .codec_ids      = codec_ids,
};
