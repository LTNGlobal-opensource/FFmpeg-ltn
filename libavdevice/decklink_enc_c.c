/*
 * Blackmagic DeckLink output
 * Copyright (c) 2013-2014 Ramiro Polla
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

#include "libavformat/avformat.h"
#include "libavutil/opt.h"

#include "decklink_common_c.h"
#include "decklink_enc.h"

#define OFFSET(x) offsetof(struct decklink_cctx, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "list_devices", "list available devices"  , OFFSET(list_devices), AV_OPT_TYPE_INT   , { .i64 = 0   }, 0, 1, ENC },
    { "list_formats", "list supported formats"  , OFFSET(list_formats), AV_OPT_TYPE_INT   , { .i64 = 0   }, 0, 1, ENC },
    { "preroll"     , "video preroll in seconds", OFFSET(preroll     ), AV_OPT_TYPE_DOUBLE, { .dbl = 0.5 }, 0, 5, ENC },
    { "discard"     , "discard initial seconds of video", OFFSET(discard), AV_OPT_TYPE_DOUBLE, { .dbl = 1.5 }, 0, 5, ENC },
    { "queue_size",   "output queue buffer size", OFFSET(queue_size  ), AV_OPT_TYPE_INT64, { .i64 = (1024 * 1024 * 1024)}, 0, INT64_MAX, ENC },
    { "afd_line"    , "VANC line to insert AFD packets", OFFSET(afd_line), AV_OPT_TYPE_INT, { .i64 = 11 }, -1, INT64_MAX, ENC },
    { "cea708_line" , "VANC line to insert CEA-708 CC packets", OFFSET(cea708_line), AV_OPT_TYPE_INT, { .i64 = 12 }, -1, INT64_MAX, ENC },
    { "scte104_line", "VANC line to insert SCTE-104 packets", OFFSET(scte104_line), AV_OPT_TYPE_INT, { .i64 = 13 }, -1, INT64_MAX, ENC },
    { "udp_monitor",  "URL to controller monitor", OFFSET(udp_monitor),  AV_OPT_TYPE_STRING, { .str = NULL}, 0, 0, ENC },
    { "use_3glevel_a","use Level A if output 3G-SDI", OFFSET(use_3glevel_a), AV_OPT_TYPE_BOOL, {.i64 = -1}, -1, 1, ENC },
    { NULL },
};

static const AVClass decklink_muxer_class = {
    .class_name = "Blackmagic DeckLink muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT,
};

AVOutputFormat ff_decklink_muxer = {
    .name           = "decklink",
    .long_name      = NULL_IF_CONFIG_SMALL("Blackmagic DeckLink output"),
    .audio_codec    = AV_CODEC_ID_PCM_S16LE,
    .video_codec    = AV_CODEC_ID_WRAPPED_AVFRAME,
    .subtitle_codec = AV_CODEC_ID_NONE,
#if CONFIG_LIBKLVANC
    .data_codec     = AV_CODEC_ID_SMPTE_2038,
#endif
    .flags          = AVFMT_NOFILE,
    .get_device_list = ff_decklink_list_output_devices,
    .priv_class     = &decklink_muxer_class,
    .priv_data_size = sizeof(struct decklink_cctx),
    .write_header   = ff_decklink_write_header,
    .write_packet   = ff_decklink_write_packet,
    .write_trailer  = ff_decklink_write_trailer,
};
