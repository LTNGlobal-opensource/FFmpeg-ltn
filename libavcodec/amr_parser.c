/*
 * Copyright (c) 2021 Paul B Mahol
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

/**
 * @file
 * AMR audio parser
 *
 * Splits packets into individual blocks.
 */

#include "libavutil/intreadwrite.h"
#include "parser.h"

static const uint8_t amrnb_packed_size[16] = {
    13, 14, 16, 18, 20, 21, 27, 32, 6, 1, 1, 1, 1, 1, 1, 1
};
static const uint8_t amrwb_packed_size[16] = {
    18, 24, 33, 37, 41, 47, 51, 59, 61, 6, 1, 1, 1, 1, 1, 1
};

typedef struct AMRParseContext {
    ParseContext pc;
    uint64_t cumulated_size;
    uint64_t block_count;
    int remaining;
} AMRParseContext;

static int amr_parse(AVCodecParserContext *s1,
                     AVCodecContext *avctx,
                     const uint8_t **poutbuf, int *poutbuf_size,
                     const uint8_t *buf, int buf_size)
{
    AMRParseContext *s = s1->priv_data;
    ParseContext *pc = &s->pc;
    int next = END_NOT_FOUND;

    *poutbuf_size = 0;
    *poutbuf = NULL;

    if (s1->flags & PARSER_FLAG_COMPLETE_FRAMES) {
        next = buf_size;
    } else {
        if (s->remaining) {
            next = s->remaining;
        } else {
            int mode = (buf[0] >> 3) & 0x0F;

            if (avctx->codec_id == AV_CODEC_ID_AMR_NB) {
                next = amrnb_packed_size[mode];
            } else if (avctx->codec_id == AV_CODEC_ID_AMR_WB) {
                next = amrwb_packed_size[mode];
            }
        }

        s->remaining = next - FFMIN(buf_size, next);
        if (s->remaining)
            next = END_NOT_FOUND;

        if (next != END_NOT_FOUND) {
            if (s->cumulated_size < UINT64_MAX - next) {
                s->cumulated_size += next;
                /* Both AMR formats have 50 frames per second */
                avctx->bit_rate = s->cumulated_size / ++s->block_count * 8 * 50;
            }
        }

        if (ff_combine_frame(pc, next, &buf, &buf_size) < 0) {
            *poutbuf      = NULL;
            *poutbuf_size = 0;
            return buf_size;
        }
    }

    s1->duration = avctx->codec_id == AV_CODEC_ID_AMR_NB ? 160 : 320;

    *poutbuf = buf;
    *poutbuf_size = buf_size;
    return next;
}

const AVCodecParser ff_amr_parser = {
    .codec_ids      = { AV_CODEC_ID_AMR_NB, AV_CODEC_ID_AMR_WB },
    .priv_data_size = sizeof(AMRParseContext),
    .parser_parse   = amr_parse,
    .parser_close   = ff_parse_close,
};
