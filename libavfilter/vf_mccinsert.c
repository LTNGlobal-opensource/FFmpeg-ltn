/*
 * MacCaption MCC VANC Insertion Filter
 * Copyright (c) 2018 LTN Global Communications, Inc.
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
 * MacCaption MCC Insertion Filter
 * @todo report error if "Time Code Rate" in MCC file doesn't match actual video
 */

#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "internal.h"
#include "libklvanc/vanc.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

typedef struct MCCContext {
    const AVClass *class;
    struct klvanc_context_s *vanc_ctx;
    char *filename;
    FILE *infile_ptr;
} MCCContext;

static int cb_EIA_708B(void *callback_context, struct klvanc_context_s *ctx,
                       struct klvanc_packet_eia_708b_s *pkt)
{
    AVFrame *frame = (AVFrame *) callback_context;
    AVFrameSideData *side_data;
    uint8_t *cc;

    if (!pkt->checksum_valid)
        return 0;

    if (!pkt->header.ccdata_present)
        return 0;

    side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_A53_CC);
    if (!side_data) {
        side_data = av_frame_new_side_data(frame, AV_FRAME_DATA_A53_CC, pkt->ccdata.cc_count * 3);
        if (side_data == NULL)
            return AVERROR(ENOMEM);
    }
    cc = side_data->data;

    for (int i = 0; i < pkt->ccdata.cc_count; i++) {
        cc[3*i] = 0xf8 | (pkt->ccdata.cc[i].cc_valid ? 0x04 : 0x00) |
                  (pkt->ccdata.cc[i].cc_type & 0x03);
        cc[3*i+1] = pkt->ccdata.cc[i].cc_data[0];
        cc[3*i+2] = pkt->ccdata.cc[i].cc_data[1];
    }

    return 0;
}

static int startswith(const char *haystack, const char *needle)
{
    if (strncmp(haystack, needle, strlen(needle)) == 0)
        return 1;
    return 0;
}

/* Insert n instances of 0xFA0000 into b and increment l */
#define ADDFA0000(b, l, n) { for (int x = 0; x < n; x++) { b[l++] = 0xFA; b[l++] = 0x00; b[l++] = 0x00; } }

static int mcc_readentry(AVFilterContext *avctx, struct MCCContext *ctx, AVFrame *frame)
{
    char line_buf[255];
    char *line;

    if (ctx->infile_ptr == NULL)
        return 0;

    while (1) {
        line = fgets(line_buf, sizeof(line_buf), ctx->infile_ptr);
        if (line == NULL)
            break;
        if (line[strlen(line) - 1] == '\n')
            line[strlen(line) - 1] = 0x00;
        if (line[strlen(line) - 1] == '\r')
            line[strlen(line) - 1] = 0x00;

        if (startswith(line, "UUID="))
            continue;
        if (startswith(line, "Creation Program="))
            continue;
        if (startswith(line, "Creation Date="))
            continue;
        if (startswith(line, "Creation Time="))
            continue;
        if (startswith(line, "Time Code Rate="))
            continue;
        if (strlen(line) == 0)
            continue;

        if (strncmp(line, "//", 2) == 0)
            continue;

        int h, m, s, f, ret;
        char val[255];
        unsigned char realbuf[255];
        int realbuf_offset=0;
        uint16_t *decoded_words;
        uint16_t decoded_offset = 0;
        ret = sscanf(line, "%d:%d:%d:%d\t%255s", &h, &m, &s, &f, val);
        if (ret != 5) {
            av_log(avctx, AV_LOG_ERROR, "Failed to parse line [%s]\n", line);
            continue;
        }

        for (int i = 0; i < strlen(val); i++) {
            unsigned int cur = 0;
            if (isxdigit(val[i]) && ((i+1) < strlen(val)) && isxdigit(val[i+1])) {
                sscanf(val + i, "%2x", &cur);
                realbuf[realbuf_offset++] = cur;
                i++;
                continue;
            }

            /* Support wonky MCC compression schema */
            switch(val[i]) {
            case 'G':
                ADDFA0000(realbuf, realbuf_offset, 1);
                break;
            case 'H':
                ADDFA0000(realbuf, realbuf_offset, 2);
                break;
            case 'I':
                ADDFA0000(realbuf, realbuf_offset, 3);
                break;
            case 'J':
                ADDFA0000(realbuf, realbuf_offset, 4);
                break;
            case 'K':
                ADDFA0000(realbuf, realbuf_offset, 5);
                break;
            case 'L':
                ADDFA0000(realbuf, realbuf_offset, 6);
                break;
            case 'M':
                ADDFA0000(realbuf, realbuf_offset, 7);
                break;
            case 'N':
                ADDFA0000(realbuf, realbuf_offset, 8);
                break;
            case 'O':
                ADDFA0000(realbuf, realbuf_offset, 9);
                break;
            case 'P':
                realbuf[realbuf_offset++] = 0xFB;
                realbuf[realbuf_offset++] = 0x80;
                realbuf[realbuf_offset++] = 0x80;
                break;
            case 'Q':
                realbuf[realbuf_offset++] = 0xFC;
                realbuf[realbuf_offset++] = 0x80;
                realbuf[realbuf_offset++] = 0x80;
                break;
            case 'R':
                realbuf[realbuf_offset++] = 0xFD;
                realbuf[realbuf_offset++] = 0x80;
                realbuf[realbuf_offset++] = 0x80;
                break;
            case 'S':
                realbuf[realbuf_offset++] = 0x96;
                realbuf[realbuf_offset++] = 0x69;
                break;
            case 'T':
                realbuf[realbuf_offset++] = 0x61;
                realbuf[realbuf_offset++] = 0x01;
                break;
            case 'U':
                realbuf[realbuf_offset++] = 0xE1;
                realbuf[realbuf_offset++] = 0x00;
                realbuf[realbuf_offset++] = 0x00;
                break;
            case 'Z':
                realbuf[realbuf_offset++] = 0x00;
                break;
            default:
                av_log(avctx, AV_LOG_ERROR, "Unrecognized character: [%c]\n", val[i]);
            }
        }

        /* If we got this far, we found a valid line containing VANC */

        /* Because the MCC file format doesn't include parity, and it discards
           the top bit of the checksum field, we cannot simply pass it through.
           So use our utility function to regenerate the payload with the correct
           VANC checksum */
        klvanc_sdi_create_payload(realbuf[1], realbuf[0], realbuf + 3,
                                  realbuf_offset - 4, &decoded_words, &decoded_offset, 10);

        /* Sanity check to ensure the lower eight bits of the computed checksum
           matches what was in the file */
        if ((decoded_words[decoded_offset - 1] & 0xff) != realbuf[realbuf_offset - 1]) {
            av_log(avctx, AV_LOG_ERROR,"Incorrect checksum: calculated=%x file=%x\n",
                   decoded_words[decoded_offset - 1] & 0xff, realbuf[realbuf_offset - 1]);
            continue;
        }

        ctx->vanc_ctx->callback_context = frame;
        ret = klvanc_packet_parse(ctx->vanc_ctx, 9, decoded_words, decoded_offset);
        if (ret < 0) {
            /* No VANC on this line */
            av_log(avctx, AV_LOG_ERROR, "VANC failed to parse\n");
            continue;
        }

        break;
    }
    return 0;
}

static int open_mcc(AVFilterContext *avctx, struct MCCContext *ctx)
{
    char line_buf[255];
    char *line;

    if (strcmp(ctx->filename, "") == 0) {
        av_log(avctx, AV_LOG_ERROR, "infile parameter not specified\n");
        return -1;
    }

    ctx->infile_ptr = fopen(ctx->filename, "r");
    if (ctx->infile_ptr == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Failed to open file\n");
        return -1;
    }

    /* First check file format line */
    line = fgets(line_buf, sizeof(line_buf), ctx->infile_ptr);
    if (line == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Failed to read File Format line\n");
        return -1;
    }
    if (line_buf[strlen(line_buf) - 1] == '\n')
        line_buf[strlen(line_buf) - 1] = 0x00;
    if (line_buf[strlen(line_buf) - 1] == '\r')
        line_buf[strlen(line_buf) - 1] = 0x00;

    if (strncmp(line, "File Format=", strlen("File Format=")) != 0) {
        av_log(avctx, AV_LOG_ERROR, "Malformed File Format line\n");
        return -1;
    }

    if (strcmp(line, "File Format=MacCaption_MCC V1.0") != 0) {
        av_log(avctx, AV_LOG_ERROR, "Unrecognized File Format\n");
        return -1;
    }
    return 0;
}

static struct klvanc_callbacks_s callbacks =
{
    NULL,
    cb_EIA_708B,
    NULL,
    NULL,
    NULL,
    NULL,
};

static int filter_frame(AVFilterLink *link, AVFrame *frame)
{
    AVFilterContext *avctx = link->dst;
    MCCContext *s = link->dst->priv;

    /* Process an entry from the MCC file, which invokes
       libklvanc, which calls the cb_EIA_708B callback to
       actually create the AVFrame side data */
    mcc_readentry(avctx, s, frame);

    return ff_filter_frame(link->dst->outputs[0], frame);
}

#define OFFSET(x) offsetof(MCCContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption mccinsert_options[] = {
    /* MCC Options */
    { "infile", "Input MCC file", OFFSET(filename), AV_OPT_TYPE_STRING, {.str = ""}, CHAR_MIN, CHAR_MAX, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(mccinsert);

static av_cold int init(AVFilterContext *avctx)
{
    MCCContext *s = avctx->priv;

    if (klvanc_context_create(&s->vanc_ctx) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create klvanc library context\n");
        return AVERROR(ENOMEM);
    }
    s->vanc_ctx->verbose = 0;
    s->vanc_ctx->callbacks = &callbacks;

    if (open_mcc(avctx, s) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to parse MCC file\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    MCCContext *s = ctx->priv;

    if (s->infile_ptr)
        fclose(s->infile_ptr);
}

static const AVFilterPad avfilter_vf_mccinsert_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_mccinsert_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_mccinsert = {
    .name        = "mccinsert",
    .description = NULL_IF_CONFIG_SMALL("Inject MCC file into for video stream"),
    .priv_size   = sizeof(MCCContext),
    .priv_class  = &mccinsert_class,
    .init          = init,
    .uninit        = uninit,
    .inputs      = avfilter_vf_mccinsert_inputs,
    .outputs     = avfilter_vf_mccinsert_outputs,
};
