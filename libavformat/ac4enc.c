/*
 * Raw AC-4 muxer
 *
 * Copyright (c) 2021 LTN Global Communications Inc.
 * derived from the adtsenc.c code by
 * Copyright (c) 2006 Baptiste Coudurier <baptiste.coudurier@smartjog.com>
 *                    Mans Rullgard <mans@mansr.com>
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

#include "libavcodec/get_bits.h"
#include "libavcodec/put_bits.h"
#include "libavcodec/codec_id.h"
#include "libavcodec/codec_par.h"
#include "libavcodec/packet.h"
#include "libavcodec/mpeg4audio.h"
#include "libavutil/crc.h"
#include "libavutil/opt.h"
#include "avformat.h"
#include "apetag.h"
#include "id3v2.h"

#define ADTS_HEADER_SIZE 7

typedef struct AC4Context {
    AVClass *class;
    int write_crc;
} AC4Context;


static int ac4_init(AVFormatContext *s)
{
    AC4Context *ac4 = s->priv_data;
    AVCodecParameters *par = s->streams[0]->codecpar;

    if (par->codec_id != AV_CODEC_ID_AC4) {
        av_log(s, AV_LOG_ERROR, "Only AC-4 streams can be muxed by the AC-4 muxer\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static int ac4_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    AC4Context *ac4 = s->priv_data;
    AVIOContext *pb = s->pb;
    PutBitContext pbc;
    uint8_t buf[4];
    uint16_t crc;

    if (!pkt->size)
        return 0;

    init_put_bits(&pbc, buf, 4);

    /* Described in ETSI TS 103 190-1 V1.3.1, Annex G */

    /* Sync word */
    if (ac4->write_crc)
        put_bits(&pbc, 16, 0xAC41);
    else
        put_bits(&pbc, 16, 0xAC40);

    /* Frame size */
    if (pkt->size > 0xffff) {
        put_bits(&pbc, 16, 0xffff);
        put_bits(&pbc, 24, pkt->size);
    } else {
        put_bits(&pbc, 16, pkt->size);
    }
    flush_put_bits(&pbc);

    avio_write(pb, buf, 4);

    avio_write(pb, pkt->data, pkt->size);

    if (ac4->write_crc) {
        crc = av_crc(av_crc_get_table(AV_CRC_16_ANSI), 0, pkt->data, pkt->size);
        avio_wl16(pb, crc);
    }

    return 0;
}

#define ENC AV_OPT_FLAG_ENCODING_PARAM
#define OFFSET(obj) offsetof(AC4Context, obj)
static const AVOption options[] = {
    { "write_crc",  "Enable checksum", OFFSET(write_crc), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, ENC},
    { NULL },
};

static const AVClass ac4_muxer_class = {
    .class_name     = "AC4 muxer",
    .item_name      = av_default_item_name,
    .option         = options,
    .version        = LIBAVUTIL_VERSION_INT,
};

const AVOutputFormat ff_ac4_muxer = {
    .name              = "ac4",
    .long_name         = NULL_IF_CONFIG_SMALL("raw AC-4"),
    .mime_type         = "audio/ac4",
    .extensions        = "ac4",
    .priv_data_size    = sizeof(AC4Context),
    .audio_codec       = AV_CODEC_ID_AC4,
    .video_codec       = AV_CODEC_ID_NONE,
    .init              = ac4_init,
    .write_packet      = ac4_write_packet,
    .priv_class        = &ac4_muxer_class,
    .flags             = AVFMT_NOTIMESTAMPS,
};
