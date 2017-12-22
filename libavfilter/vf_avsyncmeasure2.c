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
 * video filter, negotiate yuv420p then analyze frame and attempt to extract a burnt in 32bit counter.
 *
 * usage:
 *  ffmpeg -y -i ../../LTN/20170329/cleanbars-and-counter.ts -vf klburnin -f null -
 *  Get a perfect binary copy, and a visual png.
 *  ffmpeg -y -i ../../LTN/20170329/cleanbars-and-counter.ts -vf klburnin=200:1 -vframes 500 new%010d.png
 */

#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "avfilter.h"
#include "internal.h"
#include "video.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include <sys/time.h>

extern struct timeval avsyncmeasure_tv;
extern uint64_t avsyncmeasure_tv_pts;

typedef struct BurnContext
{
	const AVClass *class;
	uint64_t framecnt;
	uint64_t totalErrors;
	uint32_t framesProcessed;
	int inError;

	/* parameters */
	uint64_t line;
	uint64_t bitwidth;
	uint64_t bitheight;
	uint64_t snapshot;

} BurnContext;

#define OFFSET(x) offsetof(BurnContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption avsyncmeasure2_options[] = {

	/* pixel row/line at which to the top of the digit box begins. */
	{ "line", "set line", OFFSET(line), AV_OPT_TYPE_INT, {.i64=200}, 1, 1080, FLAGS, "line" },
	{ "snapshot", "extract each frame to disk as YUV420P", OFFSET(snapshot), AV_OPT_TYPE_INT, {.i64=0}, 0, 1, FLAGS, "snapshot" },

	/* With and height of each bit in pixels, usually digits are 30x30 pixels. */
	{ "bitwidth", "set bit width", OFFSET(bitwidth), AV_OPT_TYPE_INT, {.i64=30}, 1, 128, FLAGS, "bitwidth" },
	{ "bitheight", "set bit height", OFFSET(bitheight), AV_OPT_TYPE_INT, {.i64=30}, 1, 128, FLAGS, "bitheight" },

	{  NULL }
};

AVFILTER_DEFINE_CLASS(avsyncmeasure2);

static int config_input(AVFilterLink *link)
{
	BurnContext *ctx = link->dst->priv;
	//const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(link->format);

	ctx->framecnt = 0;
	ctx->totalErrors = 0;
	ctx->framesProcessed = 0;
	ctx->inError = 1;

	return 0;
}

static int query_formats(AVFilterContext *ctx)
{
	AVFilterFormats *formats = NULL;
	int fmt;

	for (fmt = 0; av_pix_fmt_desc_get(fmt); fmt++) {
		//printf("fmt = %s\n", av_get_pix_fmt_name(fmt));
		//const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
		if (fmt != AV_PIX_FMT_YUV420P)
			continue;
		int ret;
#if 0
		if (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)
			continue;
#endif
		if ((ret = ff_add_format(&formats, fmt)) < 0)
			return ret;
	}

	return ff_set_common_formats(ctx, formats);
}

static void analyzeFrame(BurnContext *ctx, AVFrame *frame, uint8_t *pic, uint32_t sizeBytes)
{
    int count = 0;
    struct timeval tv;
#if 1
	for (int i = 0; i < 10; i++) {
//		printf("%02x ", pic[i]);
                if (pic[i] == 0x10)
                    count++;
        }
//		printf("%02x ", *(pic + i));
//	printf("\n");
#endif

        if (count > 5) {
#if 0
            /* Found black video */
            gettimeofday(&tv, NULL);
//            int microseconds = (avsyncmeasure_tv.tv_sec - tv.tv_sec) * 1000000 + ((int) avsyncmeasure_tv.tv_usec - (int)tv.tv_usec);
            int microseconds = (tv.tv_sec - avsyncmeasure_tv.tv_sec) * 1000000 + ((int) tv.tv_usec - (int)avsyncmeasure_tv.tv_usec);
            struct timeval tv3;
            tv3.tv_sec = microseconds/1000000;
            tv3.tv_usec = microseconds%1000000;
            printf("%ld.%06d black frame hit audio=%ld.%06d delta=%ld.%06d. pts=%lld\n", tv.tv_sec, tv.tv_usec,
                   avsyncmeasure_tv.tv_sec, avsyncmeasure_tv.tv_usec,
                   tv3.tv_sec, tv3.tv_usec, av_rescale(frame->pts, 1000000, 90000));
#else
            printf("black frame hit audio=%lld delta=%lld\n", avsyncmeasure_tv_pts,
                   avsyncmeasure_tv_pts - av_rescale(frame->pts, 1000000, 90000));
#endif
        }

        return;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
	BurnContext *ctx = inlink->dst->priv;

	//printf("%s:%s(ctx=%p)\n", __FILE__, __func__, ctx);

	AVFilterLink *outlink = inlink->dst->outputs[0];
	AVFrame *out = ff_get_video_buffer(outlink, in->width, in->height);
	if (!out) {
		av_frame_free(&in);
		return AVERROR(ENOMEM);
	}

	av_frame_copy_props(out, in);
	av_frame_copy(out, in);

	analyzeFrame(ctx, out, out->data[0], (out->width * out->height) + ((out->width * out->height) / 2));

	av_frame_free(&in);
	return ff_filter_frame(outlink, out);
}

static const AVFilterPad avfilter_vf_avsyncmeasure2_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
	.config_props = config_input,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_avsyncmeasure2_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_avsyncmeasure2 = {
    .name        = "avsyncmeasure2",
    .description = NULL_IF_CONFIG_SMALL("Copy the input video, burn in a 32bit counter and output."),
    .priv_size   = sizeof(BurnContext),
    .priv_class  = &avsyncmeasure2_class,
    .inputs      = avfilter_vf_avsyncmeasure2_inputs,
    .outputs     = avfilter_vf_avsyncmeasure2_outputs,
    .query_formats = query_formats,
};
