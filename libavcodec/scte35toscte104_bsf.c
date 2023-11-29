/*
 * SCTE-35 to SCTE-104 bitstream filter
 * Copyright (c) 2017 LTN Global Communications, Inc.
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

#include "libklscte35/scte35.h"

static int scte35toscte104_init(AVBSFContext *ctx)
{
    ctx->par_out->codec_id = AV_CODEC_ID_SCTE_104;
    return 0;
}

static int scte35toscte104_filter(AVBSFContext *ctx, AVPacket *out)
{
    AVPacket *in;
    struct scte35_splice_info_section_s *s;
    const AVTransportTimestamp *transport_ts;
    int64_t orig_pts;
    uint8_t *buf = NULL;
    uint16_t byteCount;
    int ret = 0;

    ret = ff_bsf_get_packet(ctx, &in);
    if (ret < 0)
        return ret;

    /* Retrieve the original PTS, which will be used to calculate the pre-roll */
    transport_ts = (AVTransportTimestamp *) av_packet_get_side_data(in, AV_PKT_DATA_TRANSPORT_TIMESTAMP, NULL);
    if (transport_ts == NULL) {
        /* We can't continue wihtout the original PTS, since we won't
           be able to calculate the preroll */
        return -1;
    }
    orig_pts = av_rescale_q(transport_ts->pts, transport_ts->time_base, (AVRational){1, 90000});
    av_log(ctx, AV_LOG_DEBUG, "pts=%" PRId64 " orig_pts=%" PRId64 "\n", in->pts, orig_pts);

    /* Parse the SCTE-35 packet */
    s = scte35_splice_info_section_parse(in->data, in->size);
    if (s == NULL) {
        av_log(ctx, AV_LOG_ERROR, "Failed to splice section.");
        return -1;
    }

    /* Convert the SCTE35 message into a SCTE104 command */
    ret = scte35_create_scte104_message(s, &buf, &byteCount, orig_pts);
    if (ret != 0) {
        av_log(ctx, AV_LOG_ERROR, "Unable to convert SCTE35 to SCTE104, ret = %d\n", ret);
	goto fail;
    } else if (byteCount == 0) {
        /* It's possible the SCTE-35 doens't actually result in a SCTE-104 message,
           for example, if it's a SCTE-35 bandwidth_reservation message.  In
           that case, just drop it on the floor */
        ret = AVERROR(EAGAIN);
        goto fail;
    }

    ret = av_new_packet(out, byteCount);
    if (ret < 0)
        goto fail;

    ret = av_packet_copy_props(out, in);
    if (ret < 0)
        goto fail;

    memcpy(out->data, buf, byteCount);

fail:
    if (s)
        scte35_splice_info_section_free(s);

    if (ret < 0)
        av_packet_unref(out);
    av_packet_free(&in);
    free(buf);

    av_log(ctx, AV_LOG_DEBUG, "returning ret=%d byteCount=%d\n", ret, byteCount);
    return ret;
}

static const enum AVCodecID codec_ids[] = {
    AV_CODEC_ID_SCTE_35, AV_CODEC_ID_NONE,
};

const FFBitStreamFilter ff_scte35toscte104_bsf = {
    .p.name         = "scte35toscte104",
    .p.codec_ids    = codec_ids,
    .init           = scte35toscte104_init,
    .filter         = scte35toscte104_filter,
};
