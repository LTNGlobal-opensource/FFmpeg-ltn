/*
 * H.26L/H.264/AVC/JVT/14496-10/... parameter set decoding
 * Copyright (c) 2003 Michael Niedermayer <michaelni@gmx.at>
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
 * H.264 / AVC / MPEG-4 part10 parameter set decoding.
 * @author Michael Niedermayer <michaelni@gmx.at>
 */

#include <inttypes.h>

#include "libavutil/imgutils.h"
#include "internal.h"
#include "mathops.h"
#include "avcodec.h"
#include "h264data.h"
#include "h264_ps.h"
#include "golomb.h"

#define MAX_LOG2_MAX_FRAME_NUM    (12 + 4)
#define MIN_LOG2_MAX_FRAME_NUM    4

#define EXTENDED_SAR       255

static const uint8_t default_scaling4[2][16] = {
    {  6, 13, 20, 28, 13, 20, 28, 32,
      20, 28, 32, 37, 28, 32, 37, 42 },
    { 10, 14, 20, 24, 14, 20, 24, 27,
      20, 24, 27, 30, 24, 27, 30, 34 }
};

static const uint8_t default_scaling8[2][64] = {
    {  6, 10, 13, 16, 18, 23, 25, 27,
      10, 11, 16, 18, 23, 25, 27, 29,
      13, 16, 18, 23, 25, 27, 29, 31,
      16, 18, 23, 25, 27, 29, 31, 33,
      18, 23, 25, 27, 29, 31, 33, 36,
      23, 25, 27, 29, 31, 33, 36, 38,
      25, 27, 29, 31, 33, 36, 38, 40,
      27, 29, 31, 33, 36, 38, 40, 42 },
    {  9, 13, 15, 17, 19, 21, 22, 24,
      13, 13, 17, 19, 21, 22, 24, 25,
      15, 17, 19, 21, 22, 24, 25, 27,
      17, 19, 21, 22, 24, 25, 27, 28,
      19, 21, 22, 24, 25, 27, 28, 30,
      21, 22, 24, 25, 27, 28, 30, 32,
      22, 24, 25, 27, 28, 30, 32, 33,
      24, 25, 27, 28, 30, 32, 33, 35 }
};

/* maximum number of MBs in the DPB for a given level */
static const int level_max_dpb_mbs[][2] = {
    { 10, 396       },
    { 11, 900       },
    { 12, 2376      },
    { 13, 2376      },
    { 20, 2376      },
    { 21, 4752      },
    { 22, 8100      },
    { 30, 8100      },
    { 31, 18000     },
    { 32, 20480     },
    { 40, 32768     },
    { 41, 32768     },
    { 42, 34816     },
    { 50, 110400    },
    { 51, 184320    },
    { 52, 184320    },
};

static void remove_pps(H264ParamSets *s, int id)
{
    av_buffer_unref(&s->pps_list[id]);
}

static void remove_sps(H264ParamSets *s, int id)
{
#if 0
    int i;
    if (s->sps_list[id]) {
        /* drop all PPS that depend on this SPS */
        for (i = 0; i < FF_ARRAY_ELEMS(s->pps_list); i++)
            if (s->pps_list[i] && ((PPS*)s->pps_list[i]->data)->sps_id == id)
                remove_pps(s, i);
    }
#endif
    av_buffer_unref(&s->sps_list[id]);
}

static inline int decode_hrd_parameters(GetBitContext *gb, AVCodecContext *avctx,
                                        SPS *sps)
{
    int cpb_count, i;
    cpb_count = get_ue_golomb_31(gb) + 1;

    if (cpb_count > 32U) {
        av_log(avctx, AV_LOG_ERROR, "cpb_count %d invalid\n", cpb_count);
        return AVERROR_INVALIDDATA;
    }

    sps->bit_rate_scale = get_bits(gb, 4); /* bit_rate_scale */
    sps->cpb_size_scale = get_bits(gb, 4); /* cpb_size_scale */
    for (i = 0; i < cpb_count; i++) {
        sps->bit_rate_value[i] = get_ue_golomb_long(gb); /* bit_rate_value_minus1 */
        sps->cpb_size_value[i] = get_ue_golomb_long(gb); /* cpb_size_value_minus1 */
        sps->cbr_flag[i] = get_bits1(gb);          /* cbr_flag */
    }
    sps->initial_cpb_removal_delay_length = get_bits(gb, 5) + 1;
    sps->cpb_removal_delay_length         = get_bits(gb, 5) + 1;
    sps->dpb_output_delay_length          = get_bits(gb, 5) + 1;
    sps->time_offset_length               = get_bits(gb, 5);
    sps->cpb_cnt                          = cpb_count;
    return 0;
}

static inline int decode_vui_parameters(GetBitContext *gb, AVCodecContext *avctx,
                                        SPS *sps)
{
    sps->aspect_ratio_info_present_flag = get_bits1(gb);

    if (sps->aspect_ratio_info_present_flag) {
        sps->aspect_ratio_idc = get_bits(gb, 8);
        if (sps->aspect_ratio_idc == EXTENDED_SAR) {
            sps->sar.num = get_bits(gb, 16);
            sps->sar.den = get_bits(gb, 16);
        } else if (sps->aspect_ratio_idc < FF_ARRAY_ELEMS(ff_h264_pixel_aspect)) {
            sps->sar = ff_h264_pixel_aspect[sps->aspect_ratio_idc];
        } else {
            av_log(avctx, AV_LOG_ERROR, "illegal aspect ratio\n");
            return AVERROR_INVALIDDATA;
        }
    } else {
        sps->sar.num =
        sps->sar.den = 0;
    }

    sps->overscan_info_present_flag = get_bits1(gb);
    if (sps->overscan_info_present_flag)
        sps->overscan_appropriate_flag = get_bits1(gb);

    sps->video_signal_type_present_flag = get_bits1(gb);
    if (sps->video_signal_type_present_flag) {
        sps->video_format = get_bits(gb, 3);
        sps->full_range = get_bits1(gb); /* video_full_range_flag */

        sps->colour_description_present_flag = get_bits1(gb);
        if (sps->colour_description_present_flag) {
            sps->color_primaries = get_bits(gb, 8); /* colour_primaries */
            sps->color_trc       = get_bits(gb, 8); /* transfer_characteristics */
            sps->colorspace      = get_bits(gb, 8); /* matrix_coefficients */

            // Set invalid values to "unspecified"
            if (!av_color_primaries_name(sps->color_primaries))
                sps->color_primaries = AVCOL_PRI_UNSPECIFIED;
            if (!av_color_transfer_name(sps->color_trc))
                sps->color_trc = AVCOL_TRC_UNSPECIFIED;
            if (!av_color_space_name(sps->colorspace))
                sps->colorspace = AVCOL_SPC_UNSPECIFIED;
        }
    }

    /* chroma_location_info_present_flag */
    sps->chroma_location_info_present_flag = get_bits1(gb);
    if (sps->chroma_location_info_present_flag) {
        /* chroma_sample_location_type_top_field */
        avctx->chroma_sample_location = get_ue_golomb(gb) + 1;
        get_ue_golomb(gb);  /* chroma_sample_location_type_bottom_field */
    }

    if (show_bits1(gb) && get_bits_left(gb) < 10) {
        av_log(avctx, AV_LOG_WARNING, "Truncated VUI\n");
        return 0;
    }

    sps->timing_info_present_flag = get_bits1(gb);
    if (sps->timing_info_present_flag) {
        unsigned num_units_in_tick = get_bits_long(gb, 32);
        unsigned time_scale        = get_bits_long(gb, 32);
        if (!num_units_in_tick || !time_scale) {
            av_log(avctx, AV_LOG_ERROR,
                   "time_scale/num_units_in_tick invalid or unsupported (%u/%u)\n",
                   time_scale, num_units_in_tick);
            sps->timing_info_present_flag = 0;
        } else {
            sps->num_units_in_tick = num_units_in_tick;
            sps->time_scale = time_scale;
        }
        sps->fixed_frame_rate_flag = get_bits1(gb);
    }

    sps->nal_hrd_parameters_present_flag = get_bits1(gb);
    if (sps->nal_hrd_parameters_present_flag)
        if (decode_hrd_parameters(gb, avctx, sps) < 0)
            return AVERROR_INVALIDDATA;
    sps->vcl_hrd_parameters_present_flag = get_bits1(gb);
    if (sps->vcl_hrd_parameters_present_flag)
        if (decode_hrd_parameters(gb, avctx, sps) < 0)
            return AVERROR_INVALIDDATA;
    if (sps->nal_hrd_parameters_present_flag ||
        sps->vcl_hrd_parameters_present_flag)
        get_bits1(gb);     /* low_delay_hrd_flag */
    sps->pic_struct_present_flag = get_bits1(gb);
    if (!get_bits_left(gb))
        return 0;
    sps->bitstream_restriction_flag = get_bits1(gb);
    if (sps->bitstream_restriction_flag) {
        sps->motion_vectors_over_pic_boundaries_flag = get_bits1(gb);     /* motion_vectors_over_pic_boundaries_flag */
        sps->max_bytes_per_pic_denom = get_ue_golomb(gb); /* max_bytes_per_pic_denom */
        sps->max_bits_per_mb_denom = get_ue_golomb(gb); /* max_bits_per_mb_denom */
        sps->log2_max_mv_length_horizontal = get_ue_golomb(gb); /* log2_max_mv_length_horizontal */
        sps->log2_max_mv_length_vertical = get_ue_golomb(gb); /* log2_max_mv_length_vertical */
        sps->num_reorder_frames = get_ue_golomb(gb);
        sps->max_dec_frame_buffering = get_ue_golomb(gb); /*max_dec_frame_buffering*/

        if (get_bits_left(gb) < 0) {
            sps->num_reorder_frames         = 0;
            sps->bitstream_restriction_flag = 0;
        }

        if (sps->num_reorder_frames > 16U
            /* max_dec_frame_buffering || max_dec_frame_buffering > 16 */) {
            av_log(avctx, AV_LOG_ERROR,
                   "Clipping illegal num_reorder_frames %d\n",
                   sps->num_reorder_frames);
            sps->num_reorder_frames = 16;
            return AVERROR_INVALIDDATA;
        }
    }

    return 0;
}

static int decode_scaling_list(GetBitContext *gb, uint8_t *factors, int size,
                                const uint8_t *jvt_list,
                                const uint8_t *fallback_list)
{
    int i, last = 8, next = 8;
    const uint8_t *scan = size == 16 ? ff_zigzag_scan : ff_zigzag_direct;
    if (!get_bits1(gb)) /* matrix not written, we use the predicted one */
        memcpy(factors, fallback_list, size * sizeof(uint8_t));
    else
        for (i = 0; i < size; i++) {
            if (next) {
                int v = get_se_golomb(gb);
                if (v < -128 || v > 127) {
                    av_log(NULL, AV_LOG_ERROR, "delta scale %d is invalid\n", v);
                    return AVERROR_INVALIDDATA;
                }
                next = (last + v) & 0xff;
            }
            if (!i && !next) { /* matrix not written, we use the preset one */
                memcpy(factors, jvt_list, size * sizeof(uint8_t));
                break;
            }
            last = factors[scan[i]] = next ? next : last;
        }
    return 0;
}

/* returns non zero if the provided SPS scaling matrix has been filled */
static int decode_scaling_matrices(GetBitContext *gb, const SPS *sps,
                                    const PPS *pps, int is_sps,
                                    uint8_t(*scaling_matrix4)[16],
                                    uint8_t(*scaling_matrix8)[64])
{
    int fallback_sps = !is_sps && sps->scaling_matrix_present;
    const uint8_t *fallback[4] = {
        fallback_sps ? sps->scaling_matrix4[0] : default_scaling4[0],
        fallback_sps ? sps->scaling_matrix4[3] : default_scaling4[1],
        fallback_sps ? sps->scaling_matrix8[0] : default_scaling8[0],
        fallback_sps ? sps->scaling_matrix8[3] : default_scaling8[1]
    };
    int ret = 0;
    if (get_bits1(gb)) {
        ret |= decode_scaling_list(gb, scaling_matrix4[0], 16, default_scaling4[0], fallback[0]);        // Intra, Y
        ret |= decode_scaling_list(gb, scaling_matrix4[1], 16, default_scaling4[0], scaling_matrix4[0]); // Intra, Cr
        ret |= decode_scaling_list(gb, scaling_matrix4[2], 16, default_scaling4[0], scaling_matrix4[1]); // Intra, Cb
        ret |= decode_scaling_list(gb, scaling_matrix4[3], 16, default_scaling4[1], fallback[1]);        // Inter, Y
        ret |= decode_scaling_list(gb, scaling_matrix4[4], 16, default_scaling4[1], scaling_matrix4[3]); // Inter, Cr
        ret |= decode_scaling_list(gb, scaling_matrix4[5], 16, default_scaling4[1], scaling_matrix4[4]); // Inter, Cb
        if (is_sps || pps->transform_8x8_mode) {
            ret |= decode_scaling_list(gb, scaling_matrix8[0], 64, default_scaling8[0], fallback[2]); // Intra, Y
            ret |= decode_scaling_list(gb, scaling_matrix8[3], 64, default_scaling8[1], fallback[3]); // Inter, Y
            if (sps->chroma_format_idc == 3) {
                ret |= decode_scaling_list(gb, scaling_matrix8[1], 64, default_scaling8[0], scaling_matrix8[0]); // Intra, Cr
                ret |= decode_scaling_list(gb, scaling_matrix8[4], 64, default_scaling8[1], scaling_matrix8[3]); // Inter, Cr
                ret |= decode_scaling_list(gb, scaling_matrix8[2], 64, default_scaling8[0], scaling_matrix8[1]); // Intra, Cb
                ret |= decode_scaling_list(gb, scaling_matrix8[5], 64, default_scaling8[1], scaling_matrix8[4]); // Inter, Cb
            }
        }
        if (!ret)
            ret = is_sps;
    }

    return ret;
}

void ff_h264_ps_uninit(H264ParamSets *ps)
{
    int i;

    for (i = 0; i < MAX_SPS_COUNT; i++)
        av_buffer_unref(&ps->sps_list[i]);

    for (i = 0; i < MAX_PPS_COUNT; i++)
        av_buffer_unref(&ps->pps_list[i]);

    av_buffer_unref(&ps->sps_ref);
    av_buffer_unref(&ps->pps_ref);

    ps->pps = NULL;
    ps->sps = NULL;
}

int ff_h264_decode_seq_parameter_set(GetBitContext *gb, AVCodecContext *avctx,
                                     H264ParamSets *ps, int ignore_truncation)
{
    AVBufferRef *sps_buf;
    int profile_idc, level_idc, constraint_set_flags = 0;
    unsigned int sps_id;
    int i, log2_max_frame_num_minus4;
    SPS *sps;
    int ret;

    sps_buf = av_buffer_allocz(sizeof(*sps));
    if (!sps_buf)
        return AVERROR(ENOMEM);
    sps = (SPS*)sps_buf->data;

    sps->data_size = gb->buffer_end - gb->buffer;
    if (sps->data_size > sizeof(sps->data)) {
        av_log(avctx, AV_LOG_DEBUG, "Truncating likely oversized SPS\n");
        sps->data_size = sizeof(sps->data);
    }
    memcpy(sps->data, gb->buffer, sps->data_size);

    profile_idc           = get_bits(gb, 8);
    constraint_set_flags |= get_bits1(gb) << 0;   // constraint_set0_flag
    constraint_set_flags |= get_bits1(gb) << 1;   // constraint_set1_flag
    constraint_set_flags |= get_bits1(gb) << 2;   // constraint_set2_flag
    constraint_set_flags |= get_bits1(gb) << 3;   // constraint_set3_flag
    constraint_set_flags |= get_bits1(gb) << 4;   // constraint_set4_flag
    constraint_set_flags |= get_bits1(gb) << 5;   // constraint_set5_flag
    skip_bits(gb, 2);                             // reserved_zero_2bits
    level_idc = get_bits(gb, 8);
    sps_id    = get_ue_golomb_31(gb);

    if (sps_id >= MAX_SPS_COUNT) {
        av_log(avctx, AV_LOG_ERROR, "sps_id %u out of range\n", sps_id);
        goto fail;
    }

    sps->sps_id               = sps_id;
    sps->time_offset_length   = 24;
    sps->profile_idc          = profile_idc;
    sps->constraint_set_flags = constraint_set_flags;
    sps->level_idc            = level_idc;
    sps->full_range           = -1;

    memset(sps->scaling_matrix4, 16, sizeof(sps->scaling_matrix4));
    memset(sps->scaling_matrix8, 16, sizeof(sps->scaling_matrix8));
    sps->scaling_matrix_present = 0;
    sps->colorspace = 2; //AVCOL_SPC_UNSPECIFIED

    if (sps->profile_idc == 100 ||  // High profile
        sps->profile_idc == 110 ||  // High10 profile
        sps->profile_idc == 122 ||  // High422 profile
        sps->profile_idc == 244 ||  // High444 Predictive profile
        sps->profile_idc ==  44 ||  // Cavlc444 profile
        sps->profile_idc ==  83 ||  // Scalable Constrained High profile (SVC)
        sps->profile_idc ==  86 ||  // Scalable High Intra profile (SVC)
        sps->profile_idc == 118 ||  // Stereo High profile (MVC)
        sps->profile_idc == 128 ||  // Multiview High profile (MVC)
        sps->profile_idc == 138 ||  // Multiview Depth High profile (MVCD)
        sps->profile_idc == 144) {  // old High444 profile
        sps->chroma_format_idc = get_ue_golomb_31(gb);
        if (sps->chroma_format_idc > 3U) {
            avpriv_request_sample(avctx, "chroma_format_idc %u",
                                  sps->chroma_format_idc);
            goto fail;
        } else if (sps->chroma_format_idc == 3) {
            sps->residual_color_transform_flag = get_bits1(gb);
            if (sps->residual_color_transform_flag) {
                av_log(avctx, AV_LOG_ERROR, "separate color planes are not supported\n");
                goto fail;
            }
        }
        sps->bit_depth_luma   = get_ue_golomb(gb) + 8;
        sps->bit_depth_chroma = get_ue_golomb(gb) + 8;
        if (sps->bit_depth_chroma != sps->bit_depth_luma) {
            avpriv_request_sample(avctx,
                                  "Different chroma and luma bit depth");
            goto fail;
        }
        if (sps->bit_depth_luma   < 8 || sps->bit_depth_luma   > 14 ||
            sps->bit_depth_chroma < 8 || sps->bit_depth_chroma > 14) {
            av_log(avctx, AV_LOG_ERROR, "illegal bit depth value (%d, %d)\n",
                   sps->bit_depth_luma, sps->bit_depth_chroma);
            goto fail;
        }
        sps->transform_bypass = get_bits1(gb);
        ret = decode_scaling_matrices(gb, sps, NULL, 1,
                                      sps->scaling_matrix4, sps->scaling_matrix8);
        if (ret < 0)
            goto fail;
        sps->scaling_matrix_present |= ret;
    } else {
        sps->chroma_format_idc = 1;
        sps->bit_depth_luma    = 8;
        sps->bit_depth_chroma  = 8;
    }

    log2_max_frame_num_minus4 = get_ue_golomb(gb);
    if (log2_max_frame_num_minus4 < MIN_LOG2_MAX_FRAME_NUM - 4 ||
        log2_max_frame_num_minus4 > MAX_LOG2_MAX_FRAME_NUM - 4) {
        av_log(avctx, AV_LOG_ERROR,
               "log2_max_frame_num_minus4 out of range (0-12): %d\n",
               log2_max_frame_num_minus4);
        goto fail;
    }
    sps->log2_max_frame_num = log2_max_frame_num_minus4 + 4;

    sps->poc_type = get_ue_golomb_31(gb);

    if (sps->poc_type == 0) { // FIXME #define
        unsigned t = get_ue_golomb(gb);
        if (t>12) {
            av_log(avctx, AV_LOG_ERROR, "log2_max_poc_lsb (%d) is out of range\n", t);
            goto fail;
        }
        sps->log2_max_poc_lsb = t + 4;
    } else if (sps->poc_type == 1) { // FIXME #define
        sps->delta_pic_order_always_zero_flag = get_bits1(gb);
        sps->offset_for_non_ref_pic           = get_se_golomb(gb);
        sps->offset_for_top_to_bottom_field   = get_se_golomb(gb);
        sps->poc_cycle_length                 = get_ue_golomb(gb);

        if ((unsigned)sps->poc_cycle_length >=
            FF_ARRAY_ELEMS(sps->offset_for_ref_frame)) {
            av_log(avctx, AV_LOG_ERROR,
                   "poc_cycle_length overflow %d\n", sps->poc_cycle_length);
            goto fail;
        }

        for (i = 0; i < sps->poc_cycle_length; i++)
            sps->offset_for_ref_frame[i] = get_se_golomb(gb);
    } else if (sps->poc_type != 2) {
        av_log(avctx, AV_LOG_ERROR, "illegal POC type %d\n", sps->poc_type);
        goto fail;
    }

    sps->ref_frame_count = get_ue_golomb_31(gb);
    if (avctx->codec_tag == MKTAG('S', 'M', 'V', '2'))
        sps->ref_frame_count = FFMAX(2, sps->ref_frame_count);
    if (sps->ref_frame_count > MAX_DELAYED_PIC_COUNT) {
        av_log(avctx, AV_LOG_ERROR,
               "too many reference frames %d\n", sps->ref_frame_count);
        goto fail;
    }
    sps->gaps_in_frame_num_allowed_flag = get_bits1(gb);
    sps->mb_width                       = get_ue_golomb(gb) + 1;
    sps->mb_height                      = get_ue_golomb(gb) + 1;

    sps->frame_mbs_only_flag = get_bits1(gb);

    if (sps->mb_height >= INT_MAX / 2U) {
        av_log(avctx, AV_LOG_ERROR, "height overflow\n");
        goto fail;
    }
    sps->mb_height *= 2 - sps->frame_mbs_only_flag;

    if (!sps->frame_mbs_only_flag)
        sps->mb_aff = get_bits1(gb);
    else
        sps->mb_aff = 0;

    if ((unsigned)sps->mb_width  >= INT_MAX / 16 ||
        (unsigned)sps->mb_height >= INT_MAX / 16 ||
        av_image_check_size(16 * sps->mb_width,
                            16 * sps->mb_height, 0, avctx)) {
        av_log(avctx, AV_LOG_ERROR, "mb_width/height overflow\n");
        goto fail;
    }

    sps->direct_8x8_inference_flag = get_bits1(gb);

#ifndef ALLOW_INTERLACE
    if (sps->mb_aff)
        av_log(avctx, AV_LOG_ERROR,
               "MBAFF support not included; enable it at compile-time.\n");
#endif
    sps->crop = get_bits1(gb);
    if (sps->crop) {
        unsigned int crop_left   = get_ue_golomb(gb);
        unsigned int crop_right  = get_ue_golomb(gb);
        unsigned int crop_top    = get_ue_golomb(gb);
        unsigned int crop_bottom = get_ue_golomb(gb);
        int width  = 16 * sps->mb_width;
        int height = 16 * sps->mb_height;

        if (avctx->flags2 & AV_CODEC_FLAG2_IGNORE_CROP) {
            av_log(avctx, AV_LOG_DEBUG, "discarding sps cropping, original "
                                           "values are l:%d r:%d t:%d b:%d\n",
                   crop_left, crop_right, crop_top, crop_bottom);

            sps->crop_left   =
            sps->crop_right  =
            sps->crop_top    =
            sps->crop_bottom = 0;
        } else {
            int vsub   = (sps->chroma_format_idc == 1) ? 1 : 0;
            int hsub   = (sps->chroma_format_idc == 1 ||
                          sps->chroma_format_idc == 2) ? 1 : 0;
            int step_x = 1 << hsub;
            int step_y = (2 - sps->frame_mbs_only_flag) << vsub;

            if (crop_left  > (unsigned)INT_MAX / 4 / step_x ||
                crop_right > (unsigned)INT_MAX / 4 / step_x ||
                crop_top   > (unsigned)INT_MAX / 4 / step_y ||
                crop_bottom> (unsigned)INT_MAX / 4 / step_y ||
                (crop_left + crop_right ) * step_x >= width ||
                (crop_top  + crop_bottom) * step_y >= height
            ) {
                av_log(avctx, AV_LOG_ERROR, "crop values invalid %d %d %d %d / %d %d\n", crop_left, crop_right, crop_top, crop_bottom, width, height);
                goto fail;
            }

            sps->crop_left   = crop_left   * step_x;
            sps->crop_right  = crop_right  * step_x;
            sps->crop_top    = crop_top    * step_y;
            sps->crop_bottom = crop_bottom * step_y;
        }
    } else {
        sps->crop_left   =
        sps->crop_right  =
        sps->crop_top    =
        sps->crop_bottom =
        sps->crop        = 0;
    }

    sps->vui_parameters_present_flag = get_bits1(gb);
    if (sps->vui_parameters_present_flag) {
        int ret = decode_vui_parameters(gb, avctx, sps);
        if (ret < 0)
            goto fail;
    }

    if (get_bits_left(gb) < 0) {
        av_log(avctx, ignore_truncation ? AV_LOG_WARNING : AV_LOG_ERROR,
               "Overread %s by %d bits\n", sps->vui_parameters_present_flag ? "VUI" : "SPS", -get_bits_left(gb));
        if (!ignore_truncation)
            goto fail;
    }

    /* if the maximum delay is not stored in the SPS, derive it based on the
     * level */
    if (!sps->bitstream_restriction_flag &&
        (sps->ref_frame_count || avctx->strict_std_compliance >= FF_COMPLIANCE_STRICT)) {
        sps->num_reorder_frames = MAX_DELAYED_PIC_COUNT - 1;
        for (i = 0; i < FF_ARRAY_ELEMS(level_max_dpb_mbs); i++) {
            if (level_max_dpb_mbs[i][0] == sps->level_idc) {
                sps->num_reorder_frames = FFMIN(level_max_dpb_mbs[i][1] / (sps->mb_width * sps->mb_height),
                                                sps->num_reorder_frames);
                break;
            }
        }
    }

    if (!sps->sar.den)
        sps->sar.den = 1;

    if (avctx->debug & FF_DEBUG_PICT_INFO) {
        static const char csp[4][5] = { "Gray", "420", "422", "444" };
        av_log(avctx, AV_LOG_DEBUG,
               "sps:%u profile:%d/%d poc:%d ref:%d %dx%d %s %s crop:%u/%u/%u/%u %s %s %"PRId32"/%"PRId32" b%d reo:%d\n",
               sps_id, sps->profile_idc, sps->level_idc,
               sps->poc_type,
               sps->ref_frame_count,
               sps->mb_width, sps->mb_height,
               sps->frame_mbs_only_flag ? "FRM" : (sps->mb_aff ? "MB-AFF" : "PIC-AFF"),
               sps->direct_8x8_inference_flag ? "8B8" : "",
               sps->crop_left, sps->crop_right,
               sps->crop_top, sps->crop_bottom,
               sps->vui_parameters_present_flag ? "VUI" : "",
               csp[sps->chroma_format_idc],
               sps->timing_info_present_flag ? sps->num_units_in_tick : 0,
               sps->timing_info_present_flag ? sps->time_scale : 0,
               sps->bit_depth_luma,
               sps->bitstream_restriction_flag ? sps->num_reorder_frames : -1
               );
    }

    /* check if this is a repeat of an already parsed SPS, then keep the
     * original one.
     * otherwise drop all PPSes that depend on it */
    if (ps->sps_list[sps_id] &&
        !memcmp(ps->sps_list[sps_id]->data, sps_buf->data, sps_buf->size)) {
        av_buffer_unref(&sps_buf);
    } else {
        remove_sps(ps, sps_id);
        ps->sps_list[sps_id] = sps_buf;
    }

    return 0;

fail:
    av_buffer_unref(&sps_buf);
    return AVERROR_INVALIDDATA;
}

static void init_dequant8_coeff_table(PPS *pps, const SPS *sps)
{
    int i, j, q, x;
    const int max_qp = 51 + 6 * (sps->bit_depth_luma - 8);

    for (i = 0; i < 6; i++) {
        pps->dequant8_coeff[i] = pps->dequant8_buffer[i];
        for (j = 0; j < i; j++)
            if (!memcmp(pps->scaling_matrix8[j], pps->scaling_matrix8[i],
                        64 * sizeof(uint8_t))) {
                pps->dequant8_coeff[i] = pps->dequant8_buffer[j];
                break;
            }
        if (j < i)
            continue;

        for (q = 0; q < max_qp + 1; q++) {
            int shift = ff_h264_quant_div6[q];
            int idx   = ff_h264_quant_rem6[q];
            for (x = 0; x < 64; x++)
                pps->dequant8_coeff[i][q][(x >> 3) | ((x & 7) << 3)] =
                    ((uint32_t)ff_h264_dequant8_coeff_init[idx][ff_h264_dequant8_coeff_init_scan[((x >> 1) & 12) | (x & 3)]] *
                     pps->scaling_matrix8[i][x]) << shift;
        }
    }
}

static void init_dequant4_coeff_table(PPS *pps, const SPS *sps)
{
    int i, j, q, x;
    const int max_qp = 51 + 6 * (sps->bit_depth_luma - 8);
    for (i = 0; i < 6; i++) {
        pps->dequant4_coeff[i] = pps->dequant4_buffer[i];
        for (j = 0; j < i; j++)
            if (!memcmp(pps->scaling_matrix4[j], pps->scaling_matrix4[i],
                        16 * sizeof(uint8_t))) {
                pps->dequant4_coeff[i] = pps->dequant4_buffer[j];
                break;
            }
        if (j < i)
            continue;

        for (q = 0; q < max_qp + 1; q++) {
            int shift = ff_h264_quant_div6[q] + 2;
            int idx   = ff_h264_quant_rem6[q];
            for (x = 0; x < 16; x++)
                pps->dequant4_coeff[i][q][(x >> 2) | ((x << 2) & 0xF)] =
                    ((uint32_t)ff_h264_dequant4_coeff_init[idx][(x & 1) + ((x >> 2) & 1)] *
                     pps->scaling_matrix4[i][x]) << shift;
        }
    }
}

static void init_dequant_tables(PPS *pps, const SPS *sps)
{
    int i, x;
    init_dequant4_coeff_table(pps, sps);
    memset(pps->dequant8_coeff, 0, sizeof(pps->dequant8_coeff));

    if (pps->transform_8x8_mode)
        init_dequant8_coeff_table(pps, sps);
    if (sps->transform_bypass) {
        for (i = 0; i < 6; i++)
            for (x = 0; x < 16; x++)
                pps->dequant4_coeff[i][0][x] = 1 << 6;
        if (pps->transform_8x8_mode)
            for (i = 0; i < 6; i++)
                for (x = 0; x < 64; x++)
                    pps->dequant8_coeff[i][0][x] = 1 << 6;
    }
}

static void build_qp_table(PPS *pps, int t, int index, const int depth)
{
    int i;
    const int max_qp = 51 + 6 * (depth - 8);
    for (i = 0; i < max_qp + 1; i++)
        pps->chroma_qp_table[t][i] =
            ff_h264_chroma_qp[depth - 8][av_clip(i + index, 0, max_qp)];
}

static int more_rbsp_data_in_pps(const SPS *sps, void *logctx)
{
    int profile_idc = sps->profile_idc;

    if ((profile_idc == 66 || profile_idc == 77 ||
         profile_idc == 88) && (sps->constraint_set_flags & 7)) {
        av_log(logctx, AV_LOG_VERBOSE,
               "Current profile doesn't provide more RBSP data in PPS, skipping\n");
        return 0;
    }

    return 1;
}

int ff_h264_decode_picture_parameter_set(GetBitContext *gb, AVCodecContext *avctx,
                                         H264ParamSets *ps, int bit_length)
{
    AVBufferRef *pps_buf;
    const SPS *sps;
    unsigned int pps_id = get_ue_golomb(gb);
    PPS *pps;
    int qp_bd_offset;
    int bits_left;
    int ret;

    if (pps_id >= MAX_PPS_COUNT) {
        av_log(avctx, AV_LOG_ERROR, "pps_id %u out of range\n", pps_id);
        return AVERROR_INVALIDDATA;
    }

    pps_buf = av_buffer_allocz(sizeof(*pps));
    if (!pps_buf)
        return AVERROR(ENOMEM);
    pps = (PPS*)pps_buf->data;

    pps->data_size = gb->buffer_end - gb->buffer;
    if (pps->data_size > sizeof(pps->data)) {
        av_log(avctx, AV_LOG_DEBUG, "Truncating likely oversized PPS "
               "(%"SIZE_SPECIFIER" > %"SIZE_SPECIFIER")\n",
               pps->data_size, sizeof(pps->data));
        pps->data_size = sizeof(pps->data);
    }
    memcpy(pps->data, gb->buffer, pps->data_size);

    pps->sps_id = get_ue_golomb_31(gb);
    if ((unsigned)pps->sps_id >= MAX_SPS_COUNT ||
        !ps->sps_list[pps->sps_id]) {
        av_log(avctx, AV_LOG_ERROR, "sps_id %u out of range\n", pps->sps_id);
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }
    sps = (const SPS*)ps->sps_list[pps->sps_id]->data;
    if (sps->bit_depth_luma > 14) {
        av_log(avctx, AV_LOG_ERROR,
               "Invalid luma bit depth=%d\n",
               sps->bit_depth_luma);
        ret = AVERROR_INVALIDDATA;
        goto fail;
    } else if (sps->bit_depth_luma == 11 || sps->bit_depth_luma == 13) {
        avpriv_report_missing_feature(avctx,
               "Unimplemented luma bit depth=%d",
               sps->bit_depth_luma);
        ret = AVERROR_PATCHWELCOME;
        goto fail;
    }

    pps->cabac             = get_bits1(gb);
    pps->pic_order_present = get_bits1(gb);
    pps->slice_group_count = get_ue_golomb(gb) + 1;
    if (pps->slice_group_count > 1) {
        pps->mb_slice_group_map_type = get_ue_golomb(gb);
        av_log(avctx, AV_LOG_ERROR, "FMO not supported\n");
    }
    pps->ref_count[0] = get_ue_golomb(gb) + 1;
    pps->ref_count[1] = get_ue_golomb(gb) + 1;
    if (pps->ref_count[0] - 1 > 32 - 1 || pps->ref_count[1] - 1 > 32 - 1) {
        av_log(avctx, AV_LOG_ERROR, "reference overflow (pps)\n");
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    qp_bd_offset = 6 * (sps->bit_depth_luma - 8);

    pps->weighted_pred                        = get_bits1(gb);
    pps->weighted_bipred_idc                  = get_bits(gb, 2);
    pps->init_qp                              = get_se_golomb(gb) + 26U + qp_bd_offset;
    pps->init_qs                              = get_se_golomb(gb) + 26U + qp_bd_offset;
    pps->chroma_qp_index_offset[0]            = get_se_golomb(gb);
    if (pps->chroma_qp_index_offset[0] < -12 || pps->chroma_qp_index_offset[0] > 12) {
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    pps->deblocking_filter_parameters_present = get_bits1(gb);
    pps->constrained_intra_pred               = get_bits1(gb);
    pps->redundant_pic_cnt_present            = get_bits1(gb);

    pps->transform_8x8_mode = 0;
    memcpy(pps->scaling_matrix4, sps->scaling_matrix4,
           sizeof(pps->scaling_matrix4));
    memcpy(pps->scaling_matrix8, sps->scaling_matrix8,
           sizeof(pps->scaling_matrix8));

    bits_left = bit_length - get_bits_count(gb);
    if (bits_left > 0 && more_rbsp_data_in_pps(sps, avctx)) {
        pps->transform_8x8_mode = get_bits1(gb);
        ret = decode_scaling_matrices(gb, sps, pps, 0,
                                pps->scaling_matrix4, pps->scaling_matrix8);
        if (ret < 0)
            goto fail;
        // second_chroma_qp_index_offset
        pps->chroma_qp_index_offset[1] = get_se_golomb(gb);
        if (pps->chroma_qp_index_offset[1] < -12 || pps->chroma_qp_index_offset[1] > 12) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }
    } else {
        pps->chroma_qp_index_offset[1] = pps->chroma_qp_index_offset[0];
    }

    build_qp_table(pps, 0, pps->chroma_qp_index_offset[0],
                   sps->bit_depth_luma);
    build_qp_table(pps, 1, pps->chroma_qp_index_offset[1],
                   sps->bit_depth_luma);

    init_dequant_tables(pps, sps);

    if (pps->chroma_qp_index_offset[0] != pps->chroma_qp_index_offset[1])
        pps->chroma_qp_diff = 1;

    if (avctx->debug & FF_DEBUG_PICT_INFO) {
        av_log(avctx, AV_LOG_DEBUG,
               "pps:%u sps:%u %s slice_groups:%d ref:%u/%u %s qp:%d/%d/%d/%d %s %s %s %s\n",
               pps_id, pps->sps_id,
               pps->cabac ? "CABAC" : "CAVLC",
               pps->slice_group_count,
               pps->ref_count[0], pps->ref_count[1],
               pps->weighted_pred ? "weighted" : "",
               pps->init_qp, pps->init_qs, pps->chroma_qp_index_offset[0], pps->chroma_qp_index_offset[1],
               pps->deblocking_filter_parameters_present ? "LPAR" : "",
               pps->constrained_intra_pred ? "CONSTR" : "",
               pps->redundant_pic_cnt_present ? "REDU" : "",
               pps->transform_8x8_mode ? "8x8DCT" : "");
    }

    remove_pps(ps, pps_id);
    ps->pps_list[pps_id] = pps_buf;

    return 0;

fail:
    av_buffer_unref(&pps_buf);
    return ret;
}


void ltn_display_sps(const H264ParamSets *ps, const char *indent)
{
    if (!ps)
        return;

    const SPS *sps = ps->sps;
    if (!sps)
        return;

    printf("%sprofile_idc               = %d\n", indent, sps->profile_idc);
    printf("%sconstraint_set0_flag      = %d\n", indent, sps->constraint_set_flags & (1 << 0));
    printf("%sconstraint_set1_flag      = %d\n", indent, sps->constraint_set_flags & (1 << 1));
    printf("%sconstraint_set2_flag      = %d\n", indent, sps->constraint_set_flags & (1 << 2));
    printf("%sconstraint_set3_flag      = %d\n", indent, sps->constraint_set_flags & (1 << 3));
    printf("%sconstraint_set4_flag      = %d\n", indent, sps->constraint_set_flags & (1 << 4));
    printf("%sconstraint_set5_flag      = %d\n", indent, sps->constraint_set_flags & (1 << 5));
    printf("%slevel_idc                 = %d\n", indent, sps->level_idc);
    printf("%ssequence_parameter_set_id = %d\n", indent, sps->sps_id);
    if (sps->profile_idc == 100 || sps->profile_idc == 110 || sps->profile_idc == 122 || sps->profile_idc == 244 ||
        sps->profile_idc == 44 || sps->profile_idc == 83 || sps->profile_idc == 86 || sps->profile_idc == 118 ||
        sps->profile_idc == 128 || sps->profile_idc == 138 || sps->profile_idc == 139 || sps->profile_idc == 134)
    {
        printf("%schroma_format_idc         = %d\n", indent, sps->chroma_format_idc);
        if (sps->chroma_format_idc == 3) {
            printf("%sresidual_color_transform_flag = %d\n", indent, sps->residual_color_transform_flag);
        }
        printf("%sbit_depth_luma            = %d\n", indent, sps->bit_depth_luma);
        printf("%sbit_depth_chroma          = %d\n", indent, sps->bit_depth_chroma);
        printf("%sbit_depth_chroma          = %d\n", indent, sps->bit_depth_chroma);
    }
    printf("%slog2_max_frame_num        = %d\n", indent, sps->log2_max_frame_num);
    printf("%spic_order_cnt_type        = %d\n", indent, sps->poc_type);
    if (sps->poc_type == 0) {
        printf("%slog2_max_pic_order_cnt_lsb_minus4 = %d\n", indent, sps->log2_max_poc_lsb - 4);
    } else
    if (sps->poc_type == 1) {
        printf("%sdelta_pic_order_always_zero_flag = %d\n", indent, sps->delta_pic_order_always_zero_flag);
        printf("%soffset_for_non_ref_pic           = %d\n", indent, sps->offset_for_non_ref_pic);
        printf("%soffset_for_top_to_bottom_field   = %d\n", indent, sps->offset_for_top_to_bottom_field);
        printf("%snum_ref_frames_in_pic_order_cnt_cycle = %d\n", indent, sps->poc_cycle_length);
        for (int i = 0; i < sps->poc_cycle_length; i++)
            printf("%soffset_for_ref_frame[%d]              = %d\n", indent, i, sps->offset_for_ref_frame[i]);
    }
    printf("%smax_num_ref_frames        = %d\n", indent, sps->ref_frame_count);
    printf("%sgaps_in_frame_num_value_allowed_flag = %d\n", indent, sps->gaps_in_frame_num_allowed_flag);
    printf("%spic_width_in_mbs_minus1   = %d [%d]\n", indent, sps->mb_width - 1, (sps->mb_width * 16));
    printf("%spic_height_in_map_units_minus1 = %d [%d]\n", indent, sps->mb_height - 1,
        ((sps->mb_height * 1) + sps->gaps_in_frame_num_allowed_flag) * 16);
    printf("%sframe_mbs_only_flag       = %d\n", indent, sps->frame_mbs_only_flag);

    printf("%sframe_cropping_flag       = %d\n", indent, sps->crop);
    if (sps->frame_mbs_only_flag) {
        /* avcodec mangles and adjusts these fields, they're not a perfect stream match. */
        printf("%sframe_crop_left_offset    = %d\n", indent, sps->crop_left);
        printf("%sframe_crop_right_offset   = %d\n", indent, sps->crop_right);
        printf("%sframe_crop_top_offset     = %d\n", indent, sps->crop_top);
        printf("%sframe_crop_bottom_offset  = %d\n", indent, sps->crop_bottom);
    }

    printf("%svui_parameters_present_flag = %d\n", indent, sps->vui_parameters_present_flag);
    if (sps->vui_parameters_present_flag) {
        /* */
        printf("%saspect_ratio_info_present_flag = %d\n", indent, sps->aspect_ratio_info_present_flag);
        if (sps->aspect_ratio_info_present_flag) {
            printf("%saspect_ratio_idc          = %d\n", indent, sps->aspect_ratio_idc);
            if (sps->aspect_ratio_idc == EXTENDED_SAR) {
                printf("%ssar_width                 = %d\n", indent, sps->sar.num);
                printf("%ssar_height                = %d\n", indent, sps->sar.den);
            }
        }
        printf("%soverscan_info_present_flag = %d\n", indent, sps->overscan_info_present_flag);
        if (sps->overscan_info_present_flag) {
            printf("%soverscan_appropriate_flag  = %d\n", indent, sps->overscan_appropriate_flag);
        }

        printf("%svideo_signal_type_present_flag = %d\n", indent, sps->video_signal_type_present_flag);
        if (sps->video_signal_type_present_flag) {
            printf("%svideo_format                   = %d\n", indent, sps->video_format);
            printf("%svideo_full_range_flag          = %d\n", indent, sps->full_range);
            printf("%scolor_description_present_flag = %d\n", indent, sps->colour_description_present_flag);
            if (sps->colour_description_present_flag) {
                printf("%scolor_primaries                = %d\n", indent, sps->color_primaries);
                printf("%stransfer_characteristics       = %d\n", indent, sps->color_trc);
                printf("%smatrix_coefficients            = %d\n", indent, sps->colorspace);
            }
        }

        printf("%schroma_loc_info_present_flag   = %d\n", indent, sps->chroma_location_info_present_flag);
        if (sps->chroma_location_info_present_flag) {
            /* Some parser changes required here to gather this. */
        }

        printf("%stiming_info_present_flag       = %d\n", indent, sps->timing_info_present_flag);
        if (sps->timing_info_present_flag) {
            printf("%snum_units_in_tick              = %d\n", indent, sps->num_units_in_tick);
            printf("%stime_scale                     = %d\n", indent, sps->time_scale);
            printf("%sfixed_frame_rate_flag          = %d\n", indent, sps->fixed_frame_rate_flag);
        }

        printf("%snal_hrd_parameters_present_flag    = %d\n", indent, sps->nal_hrd_parameters_present_flag);
        if (sps->nal_hrd_parameters_present_flag) {
            printf("%scpb_count                          = %d\n", indent, sps->cpb_cnt);
            printf("%sbit_rate_scale                     = %d\n", indent, sps->bit_rate_scale);
            printf("%scpb_size_scale                     = %d\n", indent, sps->cpb_size_scale);
            for (int i = 0; i < sps->cpb_cnt; i++) {
              printf("%sbit_rate_value[%2d]                = %d\n", indent, i, sps->bit_rate_value[i]);
              printf("%scpb_size_value[%2d]                = %d\n", indent, i, sps->cpb_size_value[i]);
              printf("%scbr_flag[%2d]                      = %d\n", indent, i, sps->cbr_flag[i]);
            }
            printf("%sinitial_cpb_removal_delay_length_minus1 = %d\n", indent, sps->initial_cpb_removal_delay_length - 1);
            printf("%scpb_removal_delay_length_minus1         = %d\n", indent, sps->cpb_removal_delay_length - 1);
            printf("%sdpb_output_delay_length_minus1          = %d\n", indent, sps->dpb_output_delay_length - 1);
            printf("%stime_offset_length                      = %d\n", indent, sps->time_offset_length);
        }

        printf("%ssps->vcl_hrd_parameters_present_flag = %d\n", indent, sps->vcl_hrd_parameters_present_flag);
        if (sps->vcl_hrd_parameters_present_flag) {
            /* TODO: A single decode func uses sahred vars, we'd need to split these. */
        }

        printf("%ssps->sps->pic_struct_present_flag    = %d\n", indent, sps->pic_struct_present_flag);
        if (sps->pic_struct_present_flag) {
            /* Nothing to do according to the spec. */
        }

        printf("%ssps->bitstream_restriction_flag      = %d\n", indent, sps->bitstream_restriction_flag);
        if (sps->bitstream_restriction_flag) {
            printf("%smotion_vectors_over_pic_boundaries_flag = %d\n", indent, sps->motion_vectors_over_pic_boundaries_flag);
            printf("%smax_bytes_per_pic_denom                 = %d\n", indent, sps->max_bytes_per_pic_denom);
            printf("%smax_bits_per_mb_denom                   = %d\n", indent, sps->max_bits_per_mb_denom);
            printf("%slog2_max_mv_length_horizontal           = %d\n", indent, sps->log2_max_mv_length_horizontal);
            printf("%slog2_max_mv_length_vertical             = %d\n", indent, sps->log2_max_mv_length_vertical);
            printf("%snum_reorder_frames                      = %d\n", indent, sps->num_reorder_frames);
            printf("%smax_dec_frame_buffering                 = %d\n", indent, sps->max_dec_frame_buffering);
        }
    } /* VUI present */
}

void ltn_display_pps(const H264ParamSets *ps, const char *indent)
{
    if (!ps)
        return;

    const PPS *pps = ps->pps;
    if (!pps)
        return;

    //printf("%spic_parameter_set_id      = %d\n", indent, pps->pic_parameter_set_id);
    printf("%sseq_parameter_set_id      = %d\n", indent, pps->sps_id);
    printf("%sentropy_coding_mode_flag  = %d (check this)\n", indent, pps->cabac);
    printf("%snum_slice_groups_minus1   = %d\n", indent, pps->slice_group_count - 1);
    if (pps->slice_group_count > 0) {
        printf("%sslice_group_map_type      = %d\n", indent, pps->mb_slice_group_map_type);
        if (pps->mb_slice_group_map_type == 0) {
            /* TODO: not in struct */
        } else
        if (pps->mb_slice_group_map_type == 2) {
            /* TODO: not in struct */
        } else
        if ((pps->mb_slice_group_map_type == 3) || (pps->mb_slice_group_map_type == 4) || (pps->mb_slice_group_map_type == 5)) {
            /* TODO: not in struct */
        } else
        if (pps->mb_slice_group_map_type == 6) {
            /* TODO: not in struct */
        }
    }
    printf("%snum_ref_idx_l0_default_active_minus1 = %d\n", indent, pps->ref_count[0]);
    printf("%snum_ref_idx_l1_default_active_minus1 = %d\n", indent, pps->ref_count[1]);
    printf("%sweighted_pred_flag                   = %d\n", indent, pps->weighted_pred);
    printf("%sweighted_bipred_idc                  = %d\n", indent, pps->weighted_bipred_idc);
    printf("%spic_init_qp_minus26                  = %d\n", indent, pps->init_qp - 26);
    printf("%spic_init_qs_minus26                  = %d\n", indent, pps->init_qs - 26);
    printf("%schroma_qp_index_offset               = %d (check this)\n", indent, pps->chroma_qp_index_offset[0]);
    printf("%sdeblocking_filter_control_present_flag = %d\n", indent, pps->deblocking_filter_parameters_present);
    printf("%sconstrained_intra_pred_flag            = %d\n", indent, pps->constrained_intra_pred);
    printf("%sredundant_pic_cnt_present_flag         = %d\n", indent, pps->redundant_pic_cnt_present);
    printf("%sredundant_pic_cnt_present_flag         = %d\n", indent, pps->redundant_pic_cnt_present);
}

