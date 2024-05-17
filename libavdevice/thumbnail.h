#ifndef THUMBNAIL_H
#define THUMBNAIL_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

typedef struct FilteringContext {
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph *filter_graph;
} FilteringContext;


typedef struct StreamContext {
    AVCodecContext *dec_ctx;
    AVCodecContext *enc_ctx;
} StreamContext;


typedef struct thumbnail_ctx {
    AVFormatContext *ofmt_ctx;
    FilteringContext *filter_ctx;
    StreamContext *stream_ctx;
    unsigned int frame_count;
} thumbnail_ctx;

int thumbnail_init(struct thumbnail_ctx *ctx, const char *out_filename,
                   unsigned int in_width, unsigned int in_height,
                   unsigned int out_width, unsigned int out_height,
                   unsigned int qscale);
int thumbnail_generate(struct thumbnail_ctx *ctx, AVPacket *packet);
int thumbnail_generate_buf(struct thumbnail_ctx *ctx, uint8_t *buf, unsigned int width, unsigned int height);
int thumbnail_shutdown(struct thumbnail_ctx *ctx);

#endif
