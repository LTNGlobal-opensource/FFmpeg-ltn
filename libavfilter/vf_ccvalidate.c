/*
 * CEA-708 Closed Caption Validator
 * Copyright (c) 2023 LTN Global Communications
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

/*
 * This is a debug tool to check that CEA-708 captions conform to the
 * specification, including checks for missing 608, malformed CCPs, etc
 *
 */

#include "avfilter.h"
#include "internal.h"
#include "video.h"
#include "libavutil/opt.h"
#include "libavutil/bprint.h"

struct cc_lookup {
    int num;
    int den;
    int cc_count;
    int num_608;
};

const static struct cc_lookup cc_lookup_vals[] = {
    { 15, 1, 40, 4 },
    { 24, 1, 25, 3 },
    { 24000, 1001, 25, 3 },
    { 30, 1, 20, 2 },
    { 30000, 1001, 20, 2},
    { 60, 1, 10, 1 },
    { 60000, 1001, 10, 1},
};

typedef struct CCValidateContext
{
    const AVClass *class;
    int side_data_present;

    int expected_cc_count;
    int expected_608;

    /* Caption monitoring */
    uint8_t ccp_sequence_num;
    uint8_t ccp[256];
    int ccp_count;
    int packet_data_size;

    /* Statistics */
    uint64_t cc12_pkt_count;
    uint64_t cc34_pkt_count;
    uint64_t cc_data_malformed;
    uint64_t incorrect_608_count;
    uint64_t services_found; /* bitmap */
    uint64_t extended_services_found; /* bitmap */
    uint64_t cc_count_errors;
    uint64_t ccp_pkt_count;
    uint64_t ccp_size_errors;
    uint64_t ccp_seq_errors;
    uint64_t sb_total_pkt_count;
    uint64_t sb_pkt_count[64];
    uint64_t sb_errors;
    uint64_t unknown_element_errors;
    int report;
    uint64_t last_dumped;
} CCValidateContext;

#define OFFSET(x) offsetof(CCValidateContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption ccvalidate_options[] = {
    { "report", "generate report every 1 second", OFFSET(report), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS },
    {  NULL }
};

AVFILTER_DEFINE_CLASS(ccvalidate);


struct element_prop {
    uint8_t val;
    const char *name;
};

struct element_prop element_names[] = {
    /* C0 */
    { 0x00, "NUL" },
    { 0x03, "ETX" },
    { 0x08, "BS" },
    { 0x0c, "FF" },
    { 0x0d, "CR" },
    { 0x0e, "HCR" },
    { 0x10, "EXT1" },
    { 0x18, "P16" },
    /* G0 */
    { 0x20, "SP" },
    /* C1 */
    { 0x80, "CW0" },
    { 0x81, "CW1" },
    { 0x82, "CW2" },
    { 0x83, "CW3" },
    { 0x84, "CW4" },
    { 0x85, "CW5" },
    { 0x86, "CW6" },
    { 0x87, "CW7" },
    { 0x88, "CLW" },
    { 0x89, "DSW" },
    { 0x8a, "HDW" },
    { 0x8b, "TGW" },
    { 0x8c, "DLW" },
    { 0x8d, "DLY" },
    { 0x8e, "DLC" },
    { 0x8f, "RST" },
    { 0x90, "SPA" },
    { 0x91, "SPC" },
    { 0x92, "SPL" },
    { 0x97, "SWA" },
    { 0x98, "DF0" },
    { 0x99, "DF1" },
    { 0x9a, "DF2" },
    { 0x9b, "DF3" },
    { 0x9c, "DF4" },
    { 0x9d, "DF5" },
    { 0x9e, "DF6" },
    { 0x9f, "DF7" },
};

static int element_len(uint8_t e) {

    /* C0 Code Set (Sec 7.1.4) */
    if (e <= 0x0f)
        return 1;
    if (e >= 0x11 && e <= 0x17)
        return 2;
    if (e >= 0x18 && e <= 0x1f)
        return 2;

    /* C1 Code Set (Sec 7.1.5, 8.10.5) */
    if (e >= 0x80 && e <= 0x87) /* CWx */
        return 1;
    if (e == 0x88) /* CLW */
        return 2;
    if (e == 0x89) /* DSW */
        return 2;
    if (e == 0x8a) /* HDW */
        return 2;
    if (e == 0x8b) /* TGW */
        return 2;
    if (e == 0x8c) /* DLW */
        return 2;
    if (e == 0x8d) /* DLY */
        return 2;
    if (e == 0x8e) /* DLC */
        return 1;
    if (e == 0x8f) /* RST */
        return 1;
    if (e == 0x90) /* SPA */
        return 3;
    if (e == 0x91) /* SPC */
        return 4;
    if (e == 0x92) /* SPL */
        return 3;
    if (e >= 0x93 && e <= 0x96) /* Unused (Sec 7.1.5.1) */
        return 2;
    if (e == 0x97) /* SWA */
        return 5;
    if (e >= 0x98 && e <= 0x9f) /* DFx */
        return 7;

    /* G0 Code Set (Sec 7.1.6) */
    if (e >= 0x20 && e <= 0x7f)
        return 1;
    /* G1 Code Set (Sec 7.1.7) */
    if (e >= 0xa0)
        return 1;

    fprintf(stderr, "Unknown element [%02x].  Assuming len 1\n", e);
    return 1;
}

static const char *element_name(uint8_t e)
{
    for (int i = 0; i < (sizeof(element_names) / sizeof(struct element_prop)); i++) {
        if (e == element_names[i].val)
            return element_names[i].name;
    }

    return NULL;
}

static void parse_sb(CCValidateContext *ctx, uint8_t *sb, unsigned int len)
{
    uint8_t c = 0;
    AVBPrint buf;

    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&buf, "SB: ");
    for (int i = 0; i < len; i++)
        av_bprintf(&buf, "%02x ", sb[i]);
    if (len == 0)
        av_bprintf(&buf, "NULL service block");
    av_log(ctx, AV_LOG_DEBUG, "%s\n", buf.str);
    av_bprint_finalize(&buf, NULL);

    while (c < len) {
        uint8_t code = sb[c];
        int elen = element_len(code);
        av_log(ctx, AV_LOG_DEBUG, "Code: %02x(%d) ", code, elen);

        const char *name = element_name(code);
        if (name)
            av_log(ctx, AV_LOG_DEBUG, "[%s] ", name);
        else if (code >= 0x20 && code <= 0x7e)
            av_log(ctx, AV_LOG_DEBUG, "[%c] ", code);
        c++;

        /* Make sure there are enough bytes remaining */
        if (c + elen - 1 > len) {
            av_log(ctx, AV_LOG_ERROR, "Error: element len=%d but only %d bytes remaining\n",
                   elen, len - c);
            ctx->sb_errors++;
        } else if (elen > 1) {
            av_log(ctx, AV_LOG_DEBUG, "Args: ");
            for (int i = 1; i < elen; i++) {
                av_log(ctx, AV_LOG_DEBUG, "%02x ", sb[c]);
                c++;
            }
        }
        av_log(ctx, AV_LOG_DEBUG, "\n");
    }
}

static void parse_ccp(CCValidateContext *ctx, uint8_t *ccp, unsigned int len)
{
    uint8_t service_num;
    uint8_t block_size;
    uint8_t sb[255];
    AVBPrint buf;
    int c = 0;


    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&buf, "CCP: ");
    for (int j = 0; j < len; j++) {
        av_bprintf(&buf, "%02x ", ccp[j]);
    }
    av_log(ctx, AV_LOG_DEBUG, "%s\n", buf.str);
    av_bprint_finalize(&buf, NULL);


    /* Iterate and extract service blocks */
    while (c < len) {
        service_num = ccp[c] >> 5;
        block_size = ccp[c] & 0x1f;
        c++;
        av_log(ctx, AV_LOG_DEBUG, "service_num=%d size=%d\n", service_num, block_size);
        if (service_num == 0x07 && block_size != 0) {
            uint8_t extended_service = ccp[c++] & 0x3f;
            av_log(ctx, AV_LOG_DEBUG, "Extended service_num=%d\n", extended_service);
            ctx->extended_services_found |= (1ULL << extended_service);
        } else {
            ctx->services_found |= (1ULL << service_num);
        }

        if (c + block_size > len) {
            av_log(ctx, AV_LOG_ERROR, "Error: block size=%d but only %d bytes remaining\n",
                   block_size, len - c);
        }

        ctx->sb_total_pkt_count++;
        ctx->sb_pkt_count[service_num]++;

        if (service_num != 0) {
            memset(sb, 0, sizeof(sb));
            for (int i = 0; i < block_size; i++) {
                sb[i] = ccp[c++];
            }
            parse_sb(ctx, sb, block_size);
        }
    }
}


static void dump_status(CCValidateContext *ctx)
{
    av_log(ctx, AV_LOG_INFO, "=== CC Validation Status ===\n");
    av_log(ctx, AV_LOG_INFO, "CEA-608 services found: %s %s\n", ctx->cc12_pkt_count ? "1 2" : "", ctx->cc34_pkt_count ? "3 4" : "");
    av_log(ctx, AV_LOG_INFO, "CEA-608 CC1/CC2 packet count: %" PRIu64 "\n", ctx->cc12_pkt_count);
    av_log(ctx, AV_LOG_INFO, "CEA-608 CC3/CC4 packet count: %" PRIu64 "\n", ctx->cc34_pkt_count);
    av_log(ctx, AV_LOG_INFO, "CEA-608 incorrect tuple count: %" PRIu64 "\n", ctx->incorrect_608_count);
    av_log(ctx, AV_LOG_INFO, "CEA-708 malformed cc_data packets: %" PRIu64 "\n", ctx->cc_data_malformed);
    av_log(ctx, AV_LOG_INFO, "CEA-708 services found:");
    for (uint64_t i = 0; i < 64; i++) {
        if (ctx->services_found & (1ULL << i))
            av_log(ctx, AV_LOG_INFO, " %" PRId64, i);
    }
    av_log(ctx, AV_LOG_INFO, "\n");
    av_log(ctx, AV_LOG_INFO, "CEA-708 extended services found:");
    for (uint64_t i = 0; i < 64; i++) {
        if (ctx->extended_services_found & (1ULL << i))
            av_log(ctx, AV_LOG_INFO, " %" PRIu64, i);
    }
    av_log(ctx, AV_LOG_INFO, "\n");
    av_log(ctx, AV_LOG_INFO, "CEA-708 CC count errors: %" PRIu64 "\n", ctx->cc_count_errors);
    av_log(ctx, AV_LOG_INFO, "CEA-708 CCP packet count: %" PRIu64 "\n", ctx->ccp_pkt_count);
    av_log(ctx, AV_LOG_INFO, "CEA-708 CCP size errors: %" PRIu64 "\n", ctx->ccp_size_errors);
    av_log(ctx, AV_LOG_INFO, "CEA-708 CCP sequence errors: %" PRIu64 "\n", ctx->ccp_seq_errors);
    av_log(ctx, AV_LOG_INFO, "CEA-708 Service Block packet count: %" PRIu64 "\n", ctx->sb_total_pkt_count);
    for (int service_num = 0; service_num < 64; service_num++) {
        if (ctx->sb_pkt_count[service_num] > 0)
            av_log(ctx, AV_LOG_INFO, "CEA-708 Service Block packet count (Service %d): %" PRIu64 "\n", service_num, ctx->sb_pkt_count[service_num]);
    }
    av_log(ctx, AV_LOG_INFO, "CEA-708 Service Block errors: %" PRIu64 "\n", ctx->sb_errors);
    av_log(ctx, AV_LOG_INFO, "CEA-708 Unknown element errors: %" PRIu64 "\n", ctx->unknown_element_errors);
}

static int config_input(AVFilterLink *link)
{
    CCValidateContext *ctx = link->dst->priv;
    int i;

    /* Based on the target FPS, figure out the expected cc_count and number of
       608 tuples per packet.  See ANSI/CTA-708-E Sec 4.3.6.1. */
    for (i = 0; i < FF_ARRAY_ELEMS(cc_lookup_vals); i++) {
        if (link->frame_rate.num == cc_lookup_vals[i].num &&
            link->frame_rate.den == cc_lookup_vals[i].den) {
            ctx->expected_cc_count = cc_lookup_vals[i].cc_count;
            ctx->expected_608 = cc_lookup_vals[i].num_608;
            break;
        }
    }

    ctx->ccp_sequence_num = 0xff;

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    CCValidateContext *ctx = inlink->dst->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    AVBPrint buf;
    uint8_t *cc_data;
    uint64_t cc_count;
    uint64_t last_dumped;
    int cea608_tuples_found = 0;
    int i;

    AVFrameSideData *side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_A53_CC);
    if (!side_data) {
        return ff_filter_frame(outlink, frame);
    }

    if (side_data->size % 3 != 0) {
        av_log(ctx, AV_LOG_ERROR, "Payload size must be divisible by 3\n");
        return ff_filter_frame(outlink, frame);
    }

    cc_data = side_data->data;
    cc_count = side_data->size / 3;
    ctx->side_data_present = 1;

    if (cc_count != ctx->expected_cc_count) {
        av_log(ctx, AV_LOG_ERROR, "CC count incorrect.  Expected=%d received=%" PRIu64 "\n", ctx->expected_cc_count, cc_count);
        ctx->cc_count_errors++;
    }

    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&buf, "CC_DATA: ");
    for (i = 0; i < side_data->size; i++)
        av_bprintf(&buf, "%02x ", cc_data[i]);
    av_log(ctx, AV_LOG_DEBUG, "%s\n", buf.str);
    av_bprint_finalize(&buf, NULL);

    /* Let's decode the CCP */

    for (int i = 0; i < cc_count; i++) {
        uint8_t *cc = &cc_data[i*3];
        uint8_t onebit = (cc[0] & 0x80) >> 7;
        uint8_t reserved = (cc[0] & 0x78) >> 3;
        uint8_t cc_valid = (cc[0] & 0x06) >> 2;
        uint8_t cc_type = cc[0] & 0x03;
        int ccp_seq, packet_size_code, expected_ccp_seq;

        if (onebit != 0x01 || reserved != 0x0f) {
            av_log(ctx, AV_LOG_ERROR, "CC data field malformed: %02x %02x %02x\n", cc[0], cc[1], cc[2]);
            ctx->cc_data_malformed++;
        }

        /* Check for presence of legacy CEA-608 tuples */
        if (cc_type == 0 && cc_valid == 1) {
            ctx->cc12_pkt_count++;
            cea608_tuples_found++;
        } else if (cc_type == 1 && cc_valid == 1) {
            ctx->cc34_pkt_count++;
            cea608_tuples_found++;
        }

        if (cc_valid) {
            if (cc_type == 0x03) {
                /* Start of new DTV packet, but first handle previous packet */
                if (ctx->ccp_count > 0) {
                    if (ctx->packet_data_size > ctx->ccp_count) {
                        av_log(ctx, AV_LOG_ERROR, "Error: incomplete CCP packet, packet_data_size=%d ccp_count=%d\n",
                               ctx->packet_data_size, ctx->ccp_count);
                        ctx->ccp_size_errors++;
                    } else {
                        parse_ccp(ctx, ctx->ccp, ctx->ccp_count);
                        ctx->ccp_pkt_count++;
                    }
                }
                ctx->ccp_count = 0;
                memset(ctx->ccp, 0, sizeof(ctx->ccp));

                ccp_seq = cc[1] >> 6;
                packet_size_code = cc[1] & 0x3f;
                if (packet_size_code == 0)
                    ctx->packet_data_size = 127;
                else
                    ctx->packet_data_size = packet_size_code * 2 - 1;
                av_log(ctx, AV_LOG_DEBUG, "CCP Sequence number: %d size=%d\n",
                        ccp_seq, ctx->packet_data_size);

                expected_ccp_seq = ctx->ccp_sequence_num + 1;
                if (expected_ccp_seq == 4)
                    expected_ccp_seq = 0;
                if (ccp_seq != expected_ccp_seq && ctx->ccp_sequence_num != 0xff) {
                    av_log(ctx, AV_LOG_ERROR, "CCP Sequence inconsistent.  Received=%d Expected=%d\n", ccp_seq, expected_ccp_seq);
                    ctx->ccp_seq_errors++;
                }
                ctx->ccp_sequence_num = ccp_seq;

                if (ctx->packet_data_size > 0)
                    ctx->ccp[ctx->ccp_count++] = cc[2];
            } else if (cc_type == 0x02) {
                /* Continuation of DTV packet */
                if (ctx->ccp_count < ctx->packet_data_size)
                    ctx->ccp[ctx->ccp_count++] = cc[1];
                if (ctx->ccp_count < ctx->packet_data_size)
                    ctx->ccp[ctx->ccp_count++] = cc[2];
            }
        }
    }

    if (cea608_tuples_found != ctx->expected_608) {
        av_log(ctx, AV_LOG_ERROR, "Incorrect number of 608 tuples.  Received=%d Expected=%d\n",
            cea608_tuples_found, ctx->expected_608);
        ctx->incorrect_608_count++;
    }

    /* Print status once for every second of video */
    last_dumped = frame->pts * inlink->frame_rate.den / inlink->frame_rate.num / 1000;
    if (ctx->report && ctx->last_dumped != last_dumped) {
        dump_status(ctx);
        ctx->last_dumped = last_dumped;
    }

    return ff_filter_frame(outlink, frame);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    CCValidateContext *s = ctx->priv;
    if (s->side_data_present)
        dump_status(s);
}

static const AVFilterPad avfilter_vf_ccvalidate_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
};

const AVFilter ff_vf_ccvalidate = {
    .name        = "ccvalidate",
    .description = NULL_IF_CONFIG_SMALL("Validate CEA-708 closed caption metadata"),
    .uninit      = uninit,
    .priv_size   = sizeof(CCValidateContext),
    .priv_class  = &ccvalidate_class,
    FILTER_INPUTS(avfilter_vf_ccvalidate_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
};

