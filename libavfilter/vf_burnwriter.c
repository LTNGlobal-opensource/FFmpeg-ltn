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

/**
 * Copyright Kernel Labs Inc. 2017 <stoth@kernellabs.com>
 *
 * @file
 * video filter, negotiate RGB32 then burn a frame counter into output frames.
 *
 * usage:
 *  Draw the boxes starting at line 300 (def: 30x30pix), and start the counter at 4096.
 *  ffmpeg -y -i ../../LTN/20170329/cleanbars-and-counter.ts -vf burnwriter=300:4096 -f mpegts new.ts
 */

#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "avfilter.h"
#include "internal.h"
#include "video.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"

typedef struct BurnContext
{
	const AVClass *class;
	uint32_t framesProcessed;
	uint32_t frameCounter;

	/* parameters */
	uint64_t line;
	uint64_t bitwidth;
	uint64_t bitheight;

} BurnContext;

#define OFFSET(x) offsetof(BurnContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption burnwriter_options[] = {

	/* pixel row/line at which to the top of the digit box begins. */
	{ "line", "set line", OFFSET(line), AV_OPT_TYPE_INT, {.i64=1}, 1, 1080, FLAGS, "line" },
	{ "initial", "set initial counter value", OFFSET(frameCounter), AV_OPT_TYPE_INT, {.i64=1}, 1, 0xffffffff, FLAGS, "frameCounter" },

	/* With and height of each bit in pixels, usually digits are 30x30 pixels. */
	{ "bitwidth", "set bit width", OFFSET(bitwidth), AV_OPT_TYPE_INT, {.i64=30}, 1, 128, FLAGS, "bitwidth" },
	{ "bitheight", "set bit height", OFFSET(bitheight), AV_OPT_TYPE_INT, {.i64=30}, 1, 128, FLAGS, "bitheight" },

	{  NULL }
};

AVFILTER_DEFINE_CLASS(burnwriter);

static int config_input(AVFilterLink *link)
{
	BurnContext *ctx = link->dst->priv;

	ctx->framesProcessed = 0;

	return 0;
}

static int query_formats(AVFilterContext *ctx)
{
	AVFilterFormats *formats = NULL;
	int fmt, ret;

	for (fmt = 0; av_pix_fmt_desc_get(fmt); fmt++) {
		if (fmt != AV_PIX_FMT_RGB32)
			continue;
		if ((ret = ff_add_format(&formats, fmt)) < 0)
			return ret;
	}

	return ff_set_common_formats(ctx, formats);
}

static void writeFrame(BurnContext *ctx, AVFrame *frame, uint8_t *pic, uint32_t sizeBytes)
{       
	/* Figure out where the vertical center of row of digits should be */

	uint8_t color;
	for (int i = ctx->line; i < (ctx->line + ctx->bitheight); i++) {
		uint8_t *p = pic + (i * (frame->width * 4));

		/* Write 32 bits */
		for (int c = 31; c >= 0; c--) {
			int bit = (ctx->frameCounter >> c) & 1;
			if (bit)
				color = 0xff;
			else
				color = 0;
			for (int z = 0; z < ctx->bitwidth; z++) {
				*p++ = color;
				*p++ = color;
				*p++ = color;
				*p++ = 0x0;
			}
		}
	}

	printf("Frame %dx%d fmt:%s buf:%p bytes:%d burned-in-frame#%08d totalframes#%08d\n",
		frame->width, frame->height, av_get_pix_fmt_name(frame->format), pic, sizeBytes,
		ctx->frameCounter, ctx->framesProcessed);

        ctx->framesProcessed++;
        ctx->frameCounter++;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
	BurnContext *ctx = inlink->dst->priv;

	AVFilterLink *outlink = inlink->dst->outputs[0];
	AVFrame *out = ff_get_video_buffer(outlink, in->width, in->height);
	if (!out) {
		av_frame_free(&in);
		return AVERROR(ENOMEM);
	}

	av_frame_copy_props(out, in);
	av_frame_copy(out, in);

	writeFrame(ctx, out, out->data[0], out->width * out->height);

	av_frame_free(&in);
	return ff_filter_frame(outlink, out);
}

static const AVFilterPad avfilter_vf_burnwriter_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
	.config_props = config_input,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_burnwriter_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_burnwriter = {
    .name        = "burnwriter",
    .description = NULL_IF_CONFIG_SMALL("Copy the input video, burn in a 32bit barcounter and output."),
    .priv_size   = sizeof(BurnContext),
    .priv_class  = &burnwriter_class,
    .inputs      = avfilter_vf_burnwriter_inputs,
    .outputs     = avfilter_vf_burnwriter_outputs,
    .query_formats = query_formats,
};
