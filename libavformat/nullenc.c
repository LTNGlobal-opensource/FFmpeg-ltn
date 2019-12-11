/*
 * RAW null muxer
 * Copyright (c) 2002 Fabrice Bellard
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

#include "avformat.h"
#include "libavformat/internal.h"

static int null_write_packet(struct AVFormatContext *s, AVPacket *pkt)
{
    av_log(s, AV_LOG_DEBUG, "Received packet index=%d PTS=%" PRId64 "\n", pkt->stream_index,
           pkt->pts);
    return 0;
}

static int null_write_header(AVFormatContext *avctx)
{
    int n;

    /* Setup streams. */
    for (n = 0; n < avctx->nb_streams; n++) {
        AVStream *st = avctx->streams[n];
        AVCodecParameters *c = st->codecpar;
	if (c->codec_type == AVMEDIA_TYPE_DATA) {
            switch(st->codecpar->codec_id) {
#if CONFIG_LIBKLVANC
            case AV_CODEC_ID_SMPTE_2038:
            case AV_CODEC_ID_SCTE_104:
                /* No specific setup required */
                break;
            case AV_CODEC_ID_SCTE_35:
#if CONFIG_SCTE35TOSCTE104_BSF
                if (ff_stream_add_bitstream_filter(st, "scte35toscte104", NULL) > 0) {
                    st->codecpar->codec_id = AV_CODEC_ID_SCTE_104;
                }
#else
                av_log(avctx, AV_LOG_ERROR, "SCTE-35 requires scte35toscte104 BSF to be available\n");
#endif
                break;
#endif
            default:
                av_log(avctx, AV_LOG_ERROR, "Unsupported data codec specified\n");
            }
	}
    }
    return 0;
}

AVOutputFormat ff_null_muxer = {
    .name              = "null",
    .long_name         = NULL_IF_CONFIG_SMALL("raw null video"),
    .audio_codec       = AV_NE(AV_CODEC_ID_PCM_S16BE, AV_CODEC_ID_PCM_S16LE),
    .video_codec       = AV_CODEC_ID_WRAPPED_AVFRAME,
    .data_codec        = AV_CODEC_ID_SMPTE_2038,
    .write_header      = null_write_header,
    .write_packet      = null_write_packet,
    .flags             = AVFMT_VARIABLE_FPS | AVFMT_NOFILE | AVFMT_NOTIMESTAMPS,
};
