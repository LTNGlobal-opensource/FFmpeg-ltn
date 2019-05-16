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

#include "avcodec.h"
#include "bsf.h"

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
    int size;
    const int64_t *orig_pts;
    uint8_t *buf = NULL;
    uint16_t byteCount;
    int ret = 0;

    ret = ff_bsf_get_packet(ctx, &in);
    if (ret < 0)
        return ret;

    /* Retrieve the original PTS, which will be used to calculate the pre-roll */
    orig_pts = (int64_t *) av_packet_get_side_data(in, AV_PKT_DATA_ORIG_PTS, &size);

    /* Parse the SCTE-35 packet */
    s = scte35_splice_info_section_parse(in->data, in->size);
    if (s == NULL) {
        fprintf(stderr, "Failed to splice section \n");
        return -1;
    }

    /* Convert the SCTE35 message into a SCTE104 command */
    ret = scte35_create_scte104_message(s, &buf, &byteCount, orig_pts ? *orig_pts : 0);
    if (ret != 0) {
        fprintf(stderr, "Unable to convert SCTE35 to SCTE104, ret = %d\n", ret);
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

    return ret;
}

static const enum AVCodecID codec_ids[] = {
    AV_CODEC_ID_SCTE_35, AV_CODEC_ID_NONE,
};

const AVBitStreamFilter ff_scte35toscte104_bsf = {
    .name           = "scte35toscte104",
    .init           = scte35toscte104_init,
    .filter         = scte35toscte104_filter,
    .codec_ids      = codec_ids,
};
