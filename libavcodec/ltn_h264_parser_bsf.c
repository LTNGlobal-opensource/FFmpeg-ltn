/*
 * This file is part of FFmpeg.
 *
 * Copyright LTN. 2018 <stoth@ltnglobal.com>
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
 * Test with:
 * ./ffmpeg -i /tmp/encoderoutput.ts -c:v copy -bsf:v ltn_h264_parser -f null -
 */

/* Based on the example: https://libav.org/documentation/doxygen/master/decode__video_8c_source.html */

#include "avcodec.h"
#include "bsf.h"
#include "h264_parser.h"

#include <sys/time.h>

typedef struct ReaderContext
{
    const AVCodec *codec;
    AVCodecParserContext *parser;
    AVCodecContext *c;
} ReaderContext;

static int ltn_h264_parser_filter(AVBSFContext *ctx, AVPacket *out)
{
    AVPacket *in;
    int i;
    int ret;
    ReaderContext *s = ctx->priv_data;
    H2645Packet pkt;
    int is_avc = 0;
    H264ParamSets ps;
    H264SEIContext sei;

    memset(&ps, 0, sizeof(ps));
    memset(&sei, 0, sizeof(sei));
    memset(&pkt, 0, sizeof(pkt));

    if (s->codec == NULL) {
        s->codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!s->codec) {
            fprintf(stderr, "%s() unable to find codec.\n", __func__);
            exit(1);
        }
    }
#if 0
    if (s->parser == NULL) {
        s->parser = av_parser_init(s->codec->id);
        if (!s->parser) {
            fprintf(stderr, "%s() codec parser failed to init.\n", __func__);
            exit(1);
        }
    }
#endif
    if (s->c == NULL) {
        s->c = avcodec_alloc_context3(s->codec);
        if (!s->c) {
            fprintf(stderr, "%s() codec parser unable to alloc a codec.\n", __func__);
            exit(1);
        }
        if (avcodec_open2(s->c, s->codec, NULL) < 0) {
            fprintf(stderr, "%s() codec parser unable to open a codec.\n", __func__);
            exit(1);
        }
    }
    ret = ff_bsf_get_packet(ctx, &in);
    if (ret < 0)
        return ret;

    ret = ff_h2645_packet_split(&pkt, in->data, in->size, NULL, is_avc, 0, AV_CODEC_ID_H264, 0);
    if (ret < 0) {
        fprintf(stderr, "Error splitting the input into NAL units.\n");
        return ret;
    }

    for (i = 0; i < pkt.nb_nals; i++) {
        H2645NAL *nal = &pkt.nals[i];

printf("nal type 0x%02x\n", nal->type);

        switch (nal->type) {
        case H264_NAL_IDR_SLICE:
            printf("H264_NAL_IDR_SLICE\n");
            if ((nal->data[1] & 0xFC) == 0x98) {
                fprintf(stderr, "Invalid inter IDR frame\n");
            }
            break;
        case H264_NAL_SLICE:
            printf("H264_NAL_SLICE\n");
            break;
        case H264_NAL_DPA:
        case H264_NAL_DPB:
        case H264_NAL_DPC:
            printf("H264_NAL_DP[AC]\n");
            break;
        case H264_NAL_SEI:
            ret = ff_h264_sei_decode(&sei, &nal->gb, &ps, s->c);
            ltn_sei_display(&sei);
            break;
        case H264_NAL_SPS: {
            GetBitContext tmp_gb = nal->gb;

            if (ff_h264_decode_seq_parameter_set(&tmp_gb, s->c, &ps, 0) < 0)
                break;
            ps.sps = (const SPS *)ps.sps_list[0]->data;
            ltn_display_sps(&ps);
            break;
        }
        case H264_NAL_PPS:
            ret = ff_h264_decode_picture_parameter_set(&nal->gb, s->c, &ps, nal->size_bits);
            ps.pps = (const PPS *)ps.pps_list[0]->data;
            ltn_display_pps(&ps);
            break;
        case H264_NAL_AUD:
        case H264_NAL_END_SEQUENCE:
        case H264_NAL_END_STREAM:
        case H264_NAL_FILLER_DATA:
        case H264_NAL_SPS_EXT:
        case H264_NAL_AUXILIARY_SLICE:
            printf("H264_NAL_OTHER\n");
            break;
        default:
            fprintf(stderr, "Unknown NAL code: %d (%d bits)\n", nal->type, nal->size_bits);
        }
    }

    return 0;
}

#if 0
static int ltn_h264_parser_filter(AVBSFContext *ctx, AVPacket *out)
{
    AVPacket *in;
    int i;
    int ret;

    ReaderContext *s = ctx->priv_data;

    if (s->codec == NULL) {
        s->codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!s->codec) {
            fprintf(stderr, "%s() unable to find codec.\n", __func__);
            exit(1);
        }
    }
    if (s->parser == NULL) {
        s->parser = av_parser_init(s->codec->id);
        if (!s->parser) {
            fprintf(stderr, "%s() codec parser failed to init.\n", __func__);
            exit(1);
        }
    }
    if (s->c == NULL) {
        s->c = avcodec_alloc_context3(s->codec);
        if (!s->c) {
            fprintf(stderr, "%s() codec parser unable to alloc a codec.\n", __func__);
            exit(1);
        }
        if (avcodec_open2(s->c, s->codec, NULL) < 0) {
            fprintf(stderr, "%s() codec parser unable to open a codec.\n", __func__);
            exit(1);
        }
    }

    ret = ff_bsf_get_packet(ctx, &in);
    if (ret < 0)
        return ret;

    printf("Processing NALS..... %d bytes total\n", in->size);

    uint8_t *ptr = NULL;
    uint32_t size = 0;
    i = 0;
    while (i < in->size) {
#if 1
        for (int j = i; j < 16; j++)
            printf("%02x ", in->data[i + j]);
        printf("\n");
#endif

        /* use the parser to split the data into frames */
        ret = av_parser_parse2(s->parser, s->c, &ptr, &size,
            &in->data[i], in->size - i, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        printf("%s() ret = %d, ptr = %p, size = %d, i = %d\n", __func__, ret, ptr, size, i);
        if (ret < 0) {
            fprintf(stderr, "%s() Error while parsing\n", __func__);
            exit(1);
        }

        if (ret > 0) {
            /* Parsing of a NAL was done, dump the current SPS. */
            H264ParseContext *p = s->parser->priv_data;
            ltn_display_sps(&p->ps);
        }

        i += ret;
    }

    av_packet_move_ref(out, in);
    av_packet_free(&in);
    return 0;
}
#endif

const AVBitStreamFilter ff_ltn_h264_parser_bsf = {
    .name           = "ltn_h264_parser",
    .priv_data_size = sizeof(ReaderContext),
    .filter         = ltn_h264_parser_filter,
};
