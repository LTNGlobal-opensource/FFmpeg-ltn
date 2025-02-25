/*
 * LTN Timestamp H.264 parser
 * Copyright (c) 2025 LTN Global Communications
 *
 * Author: Devin Heitmueller <dheitmueller@ltnglobal.com>
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

#include "libavutil/sei-timestamp.h"
#include "libavformat/ltnlog.h"

#include "bsf.h"
#include "bsf_internal.h"
#include "cbs_bsf.h"
#include "cbs_sei.h"

typedef struct H264MetadataContext {
    CBSBSFContext common;
} H264MetadataContext;

static int h264_ltn_timestamp_update_fragment(AVBSFContext *bsf, AVPacket *pkt,
                                         CodedBitstreamFragment *au)
{
    H264MetadataContext *ctx = bsf->priv_data;
    SEIRawMessage *message = NULL;

    while (ff_cbs_sei_find_message(ctx->common.output, au,
				   SEI_TYPE_USER_DATA_UNREGISTERED,
                                   &message) == 0) {
	    SEIRawUserDataUnregistered *payload = message->payload;

	    if (memcmp(ltn_uuid_sei_timestamp, payload->uuid_iso_iec_11578, 16) == 0) {
                    struct timeval now, diff;
                    struct timeval encode_input, encode_output;
                    int64_t val;

                    memset(&encode_input, 0, sizeof(struct timeval));
                    memset(&encode_output, 0, sizeof(struct timeval));
                    sei_timestamp_value_timeval_query(payload->data, payload->data_length, 2, &encode_input);
                    sei_timestamp_value_timeval_query(payload->data, payload->data_length, 8, &encode_output);
                    gettimeofday(&now, NULL);

                    if (encode_output.tv_sec != 0) {
                        sei_timeval_subtract(&diff, &encode_output, &encode_input);
                        val = (diff.tv_sec * 1000) + (diff.tv_usec / 1000);
                    } else {
                        val = -1;
                    }
                    ltnlog_stat("ENCODETOTAL_MS", val);
		    av_log(bsf, AV_LOG_DEBUG, "Encode: %ld ms\n", val);

                    sei_timeval_subtract(&diff, &now, &encode_input);
                    val = (diff.tv_sec * 1000) + (diff.tv_usec / 1000);
                    ltnlog_stat("GLASSTOGLASS_MS", val);
		    av_log(bsf, AV_LOG_DEBUG, "Glass to glass: %ld ms\n", val);
	    }
    }

    return 0;
}

static const CBSBSFType h264_ltn_timestamp_type = {
    .codec_id        = AV_CODEC_ID_H264,
    .fragment_name   = "access unit",
    .unit_name       = "NAL unit",
    .update_fragment = &h264_ltn_timestamp_update_fragment,
};

static int h264_ltn_timestamp_init(AVBSFContext *bsf)
{
    return ff_cbs_bsf_generic_init(bsf, &h264_ltn_timestamp_type);
}

static const AVClass h264_ltn_timestamp_class = {
    .class_name = "h264_ltn_timestamp_bsf",
    .item_name  = av_default_item_name,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const enum AVCodecID h264_ltn_timestamp_codec_ids[] = {
    AV_CODEC_ID_H264, AV_CODEC_ID_NONE,
};

const FFBitStreamFilter ff_h264_ltn_timestamp_bsf = {
    .p.name         = "h264_ltn_timestamp",
    .p.codec_ids    = h264_ltn_timestamp_codec_ids,
    .p.priv_class   = &h264_ltn_timestamp_class,
    .priv_data_size = sizeof(H264MetadataContext),
    .init           = &h264_ltn_timestamp_init,
    .close          = &ff_cbs_bsf_generic_close,
    .filter         = &ff_cbs_bsf_generic_filter,
};
