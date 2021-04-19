/*
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
 * Repackage CEA-708 arrays, which deals with incorrect cc_count for a given
 * output framerate, and incorrect 708 padding.
 *
 * See CEA CEA-10-A "EIA-708-B Implementation Guidance", Section 26.5
 * "Grouping DTVCC Data Within user_data() Structure"
 */

#include "avfilter.h"
#include "internal.h"
#include "libavutil/opt.h"
#include "libavutil/cc_fifo.h"

typedef struct CCRepackContext
{
    const AVClass *class;
    AVCCFifo *cc_fifo;
} CCRepackContext;

static const AVOption ccrepack_options[] = {
    {  NULL }
};

AVFILTER_DEFINE_CLASS(ccrepack);

static int config_input(AVFilterLink *link)
{
    CCRepackContext *ctx = link->dst->priv;

    if (!(ctx->cc_fifo = av_cc_fifo_alloc(&link->frame_rate, ctx)))
        av_log(ctx, AV_LOG_VERBOSE, "Failure to setup CC FIFO queue.  Captions will be passed through\n");

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    CCRepackContext *ctx = inlink->dst->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];

    av_cc_enqueue_avframe(ctx->cc_fifo, frame);
    av_cc_dequeue_avframe(ctx->cc_fifo, frame);

    return ff_filter_frame(outlink, frame);
}

static const AVFilterPad avfilter_vf_ccrepack_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
	.config_props = config_input,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_ccrepack_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_ccrepack = {
    .name        = "ccrepack",
    .description = NULL_IF_CONFIG_SMALL("Repack CEA-708 closed caption metadata"),
    .priv_size   = sizeof(CCRepackContext),
    .priv_class  = &ccrepack_class,
    .inputs      = avfilter_vf_ccrepack_inputs,
    .outputs     = avfilter_vf_ccrepack_outputs,
};
