/*
 * CEA-708 Closed Caption Repacker
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
 *
 * Use libzvbi's CEA-608 decoder, and report the decoded text back to the UDP monitor
 * (this is used for out-of-band rendering of captions)
 *
 */

#include "avfilter.h"
#include "filters.h"
#include "video.h"
#include "libavutil/opt.h"
#include "libavutil/avstring.h"
#include "libavformat/ltnlog.h"
#include "libzvbi.h"

typedef struct CcreportContext
{
    const AVClass *class;
    vbi_decoder *vbi;
    double last_timestamp;
} CcreportContext;

static const AVOption ccreport_options[] = {
    {  NULL }
};

AVFILTER_DEFINE_CLASS(ccreport);

static int config_input(AVFilterLink *link)
{
    CcreportContext *ctx = link->dst->priv;

    ctx->vbi = vbi_decoder_new ();
    if (ctx->vbi == NULL) {
        av_log(ctx, AV_LOG_ERROR, "Failure to setup VBI decoder instance\n");
        return -1;
    }

    return 0;
}

static void fill_rows(char *buf, size_t buflen, size_t numrows, size_t numcols)
{
    for (int row = 0; row < numrows; row++) {
        for (int col = 0; col < numcols; col++) {
            av_strlcat(buf, " ", buflen);
        }
        av_strlcat(buf, "\\n", buflen);
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    CcreportContext *ctx = inlink->dst->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    AVFrameSideData *side_data;
    vbi_page page;
    vbi_sliced sliced[2];
    int sliced_count = 0;
    vbi_bool success;
    double timestamp = 0;
    char buf[1024];
    char cc_num[4];

    side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_A53_CC);
    if (side_data == NULL || side_data->size < 3)
        return ff_filter_frame(outlink, frame);

    /* Feed the CC samples to the decoder */
    for (int i = 0; i < (side_data->size / 3); i += 3) {
        if (side_data->data[i] == 0xfc) {
            sliced[sliced_count].id = VBI_SLICED_CAPTION_525_F1;
            sliced[sliced_count].line = 21;
            sliced[sliced_count].data[0] = side_data->data[i + 1];
            sliced[sliced_count].data[1] = side_data->data[i + 2];
            sliced_count++;
        } else if (side_data->data[i] == 0xfd) {
            sliced[sliced_count].id = VBI_SLICED_CAPTION_525_F2;
            sliced[sliced_count].line = 284;
            sliced[sliced_count].data[0] = side_data->data[i + 1];
            sliced[sliced_count].data[1] = side_data->data[i + 2];
            sliced_count++;
        }
    }
    if (sliced_count > 0)
        vbi_decode (ctx->vbi, sliced, sliced_count, timestamp);
    ctx->last_timestamp = timestamp;
    timestamp = ctx->last_timestamp + 0.033; /* FIXME */

    /* Render the page */
    for (int i = 0; i < 4; i++) {
        success = vbi_fetch_cc_page (ctx->vbi, &page, i + 1, TRUE);
        if (success && page.dirty.y1 != -1) {
            int row = 0, column = 0;
            memset(buf, 0, sizeof(buf));

            /* Fill up to the start of lines provided by the zvbi renderer */
            fill_rows(buf, sizeof(buf), page.dirty.y0 - row, page.columns);

            for (row = page.dirty.y0; row <= page.dirty.y1; ++row) {
                const vbi_char *cp = page.text + row * page.columns;
                for (column = 0; column < page.columns; ++column) {
                    if (cp->unicode == '"') {
                        av_strlcatf(buf, sizeof(buf), "\\\"");
                    } else {
                        char utf8_buf[5] = "";
                        char tmp;
                        int utf8_count = 0;
                        PUT_UTF8(cp->unicode, tmp, utf8_buf[utf8_count++] = tmp;);
                        av_strlcatf(buf, sizeof(buf), "%s", utf8_buf);
                    }
                    cp++;
                }
                av_strlcat(buf, "\\n", sizeof(buf));
            }

            /* Fill any remaining lines not provided by the zvbi renderer */
            fill_rows(buf, sizeof(buf), page.rows - row, page.columns);

            snprintf(cc_num, sizeof(cc_num), "CC%d", i + 1);
            ltnlog_msg(cc_num, "%s", buf);
            av_log(ctx, AV_LOG_DEBUG, "%s=%s", cc_num, buf);
        }
    }

    /* Pass through the original frame unmodified */
    return ff_filter_frame(outlink, frame);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    CcreportContext *s = ctx->priv;
    if (s->vbi)
        vbi_decoder_delete(s->vbi);
}

static const AVFilterPad avfilter_vf_ccreport_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
};

const AVFilter ff_vf_ccreport = {
    .name        = "ccreport",
    .description = NULL_IF_CONFIG_SMALL("Report CEA-608/708 captions back to LTN controller"),
    .uninit      = uninit,
    .priv_size   = sizeof(CcreportContext),
    .priv_class  = &ccreport_class,
    FILTER_INPUTS(avfilter_vf_ccreport_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
};
