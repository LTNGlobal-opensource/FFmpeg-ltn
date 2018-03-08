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
#include "internal.h"

#include <sys/time.h>

typedef struct ReaderContext
{
    const AVCodec *codec;
    AVCodecParserContext *parser;
    AVCodecContext *c;
} ReaderContext;

static const char *slice_type_description(uint32_t type)
{
	/* Specification table 7-6 */
	switch (type) {
	case 0:	 return "P";
	case 1:	 return "B";
	case 2:	 return "I";
	case 3:	 return "SP";
	case 4:	 return "SI";
	case 5:	 return "P";
	case 6:	 return "B";
	case 7:	 return "I";
	case 8:	 return "SP";
	case 9:	 return "SI";
	default: return "UNDEFINED";
	}
}

static const char *primary_pic_type_description(uint32_t type)
{
	/* Specification table 7-5 */
	switch (type) {
	case 0:	 return "I";
	case 1:	 return "P, I";
	case 2:	 return "P, B, I";
	case 3:	 return "SI";
	case 4:	 return "SP, SI";
	case 5:	 return "I, SI";
	case 6:	 return "P, I, SP, SI";
	case 7:	 return "P, B, I, SP, SI";
	default: return "UNDEFINED";
	}
};

#define decode_slice_header()											\
{														\
            uint32_t first_mb_in_slice = get_ue_golomb_long(&nal->gb); 						\
            uint32_t slice_type = get_ue_golomb_31(&nal->gb);							\
            uint32_t pic_parameter_set_id = get_ue_golomb(&nal->gb);						\
            printf("\t\tslice_header()\n");									\
            printf("\t\t\tfirst_mb_in_slice    = %d\n", first_mb_in_slice);					\
            printf("\t\t\tslice_type           = %d [%s]\n", slice_type, slice_type_description(slice_type));	\
            printf("\t\t\tpic_parameter_set_id = %d\n", pic_parameter_set_id);					\
};

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

	printf("\n");

        switch (nal->type) {
        case H264_NAL_IDR_SLICE:
            printf("nal_type = %02x = H264_NAL_IDR_SLICE (Coded slice of an IDR Picture) -- %d bytes\n", nal->type,
                nal->size_bits / 8);
            printf("\tslice_layer_without_partitioning_rbsp()\n");

            decode_slice_header();

            break;
        case H264_NAL_SLICE:
            printf("nal_type = %02x = H264_NAL_SLICE (Coded Slice of a non-IDR Picture) -- %d bytes\n", nal->type,
                nal->size_bits / 8);
            printf("\tslice_layer_without_partitioning_rbsp()\n");

            decode_slice_header();

            break;
        case H264_NAL_DPA:
            printf("nal_type = %02x = H264_NAL_DPA (Coded Slice Data Partition A)\n", nal->type);
            break;
        case H264_NAL_DPB:
            printf("nal_type = %02x = H264_NAL_DPB (Coded Slice Data Partition B)\n", nal->type);
            break;
        case H264_NAL_DPC:
            printf("nal_type = %02x = H264_NAL_DPC (Coded Slice Data Partition C)\n", nal->type);
            break;
        case H264_NAL_SEI:
            printf("nal_type = %02x = H264_NAL_SEI (Supplemental Enhancement Information)\n", nal->type);
            printf("\tsei_rbsp()\n");
            ret = ff_h264_sei_decode(&sei, &nal->gb, &ps, s->c);
            ltn_sei_display(&sei, "\t\t");
            break;
        case H264_NAL_SPS: {
            printf("nal_type = %02x = H264_NAL_SPS (Sequence Parameter Set)\n", nal->type);
            printf("\tseq_parameter_set_rbps()\n");

            printf("\t");
            for (int i = 0; i < nal->size_bits / 8; i++) {
                printf("%02x ", nal->data[i]);
            }
            printf("\n");

            GetBitContext tmp_gb = nal->gb;

            if (ff_h264_decode_seq_parameter_set(&tmp_gb, s->c, &ps, 0) < 0)
                break;
            ps.sps = (const SPS *)ps.sps_list[0]->data;
            ltn_display_sps(&ps, "\t\t");
            break;
        }
        case H264_NAL_PPS:
            printf("nal_type = %02x = H264_NAL_PPS (Picture Parameter Set)\n", nal->type);
            printf("\tpic_parameter_set_rbps()\n");

            printf("\t");
            for (int i = 0; i < nal->size_bits / 8; i++) {
                printf("%02x ", nal->data[i]);
            }
            printf("\n");

            ret = ff_h264_decode_picture_parameter_set(&nal->gb, s->c, &ps, nal->size_bits);
            ps.pps = (const PPS *)ps.pps_list[0]->data;
            ltn_display_pps(&ps, "\t\t");
            break;
        case H264_NAL_AUD:
            printf("nal_type = %02x = H264_NAL_AUD (Access Unit Delimiter)\n", nal->type);
            printf("\taccess_unit_delimiter_rbsp()\n");
            uint8_t primary_pic_type = get_bits(&nal->gb, 3);
            printf("\t\tprimary_pic_type = 0x%x [%s]\n",
                primary_pic_type,
                primary_pic_type_description(primary_pic_type));
            break;
        case H264_NAL_END_SEQUENCE:
            printf("nal_type = %02x = H264_NAL_END_SEQUENCE (End of Sequence)\n", nal->type);
            printf("\tend_of_seq_rbsp()\n");
            break;
        case H264_NAL_END_STREAM:
            printf("nal_type = %02x = H264_NAL_END_STREAM (End of Stream)\n", nal->type);
            printf("\tend_of_stream_rbsp()\n");
            break;
        case H264_NAL_FILLER_DATA:
            printf("nal_type = %02x = H264_NAL_FILLER_DATA (Filler Data0\n", nal->type);
            printf("\tfiller_data()\n");
            break;
        case H264_NAL_SPS_EXT:
            printf("nal_type = %02x = H264_NAL_SPS_EXT (Sequence Parameter Set Extension)\n", nal->type);
            printf("\tseq_parameter_set_extension_rbsp()\n");
            break;
        case H264_NAL_AUXILIARY_SLICE:
            printf("nal_type = %02x = H264_NAL_AUXILIARY_SLICE\n", nal->type);
            break;
        default:
            fprintf(stderr, "nal_type = %02x = UNKNOWN (%d bits)\n", nal->type, nal->size_bits);
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
