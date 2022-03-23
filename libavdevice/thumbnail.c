/*
 * Copyright (c) 2010 Nicolas George
 * Copyright (c) 2011 Stefano Sabatini
 * Copyright (c) 2014 Andrey Utkin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * API example for demuxing, decoding, filtering, encoding and muxing
 * @example transcoding.c
 */

#include "thumbnail.h"


static int open_input_file(struct thumbnail_ctx *ctx, const char *filename,
                           unsigned int in_width, unsigned int in_height)
{
    int ret;
    AVCodec *dec;
    AVCodecParameters *codecpar = avcodec_parameters_alloc();
    AVCodecContext *codec_ctx;

    if (codecpar == NULL) {
        return -1;
    }

    ctx->stream_ctx = av_mallocz(sizeof(*ctx->stream_ctx));
    if (!ctx->stream_ctx)
        return AVERROR(ENOMEM);

    dec = avcodec_find_decoder(AV_CODEC_ID_V210);
    if (!dec) {
        av_log(NULL, AV_LOG_ERROR, "Failed to find decoder for stream\n");
        return AVERROR_DECODER_NOT_FOUND;
    }
    codec_ctx = avcodec_alloc_context3(dec);
    if (!codec_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Failed to allocate the decoder context for stream\n");
        return AVERROR(ENOMEM);
    }

    codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    codecpar->codec_id = AV_CODEC_ID_V210;
    codecpar->format = AV_PIX_FMT_YUV422P10;
    codecpar->width = in_width;
    codecpar->height = in_height;

    ret = avcodec_parameters_to_context(codec_ctx, codecpar);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to copy decoder parameters to input decoder context "
               "for stream\n");
        return ret;
    }

    /* The values here don't really matter */
    codec_ctx->framerate.num = 30000;
    codec_ctx->framerate.den = 1001;

    /* Open decoder */
    ret = avcodec_open2(codec_ctx, dec, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream\n");
        return ret;
    }
    ctx->stream_ctx->dec_ctx = codec_ctx;

    return 0;
}

static int open_output_file(struct thumbnail_ctx *ctx, const char *filename,
                            unsigned int out_width, unsigned int out_height,
                            unsigned int qscale)
{
    AVStream *out_stream;
    AVCodecContext *dec_ctx, *enc_ctx;
    AVCodec *encoder;
    AVDictionary *opt = NULL;
    int ret;

    ctx->ofmt_ctx = NULL;
    avformat_alloc_output_context2(&ctx->ofmt_ctx, NULL, NULL, filename);
    if (!ctx->ofmt_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
        return AVERROR_UNKNOWN;
    }

    out_stream = avformat_new_stream(ctx->ofmt_ctx, NULL);
    if (!out_stream) {
        av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
        return AVERROR_UNKNOWN;
    }

    dec_ctx = ctx->stream_ctx->dec_ctx;

    encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!encoder) {
        av_log(NULL, AV_LOG_FATAL, "Necessary encoder not found\n");
        return AVERROR_INVALIDDATA;
    }
    enc_ctx = avcodec_alloc_context3(encoder);
    if (!enc_ctx) {
        av_log(NULL, AV_LOG_FATAL, "Failed to allocate the encoder context\n");
        return AVERROR(ENOMEM);
    }

    enc_ctx->height = out_height;
    enc_ctx->width = out_width;

    /* FIXME? */
    enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;

    /* Impacts the quality of the produced JPG images */
    enc_ctx->flags |= AV_CODEC_FLAG_QSCALE;
    enc_ctx->global_quality = FF_QP2LAMBDA * qscale;

    /* take first format from list of supported formats */
    if (encoder->pix_fmts)
        enc_ctx->pix_fmt = encoder->pix_fmts[0];
    else
        enc_ctx->pix_fmt = dec_ctx->pix_fmt;
    /* video time_base can be set to whatever is handy and supported by encoder */
    enc_ctx->time_base = av_inv_q(dec_ctx->framerate);

    /* Third parameter can be used to pass settings to encoder */
    ret = avcodec_open2(enc_ctx, encoder, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open video encoder for stream\n");
        return ret;
    }
    ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to copy encoder parameters to output stream\n");
        return ret;
    }
    if (ctx->ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    out_stream->time_base = enc_ctx->time_base;
    ctx->stream_ctx->enc_ctx = enc_ctx;

    if (!(ctx->ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ctx->ofmt_ctx->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'", filename);
            return ret;
        }
    }

    av_dict_set_int(&opt, "update", 1, 0);

    /* init muxer, write output file header */
    ret = avformat_write_header(ctx->ofmt_ctx, &opt);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
        return ret;
    }

    return 0;
}

static int init_filter(struct thumbnail_ctx *ctx,
                       FilteringContext* fctx, AVCodecContext *dec_ctx,
                       AVCodecContext *enc_ctx, const char *filter_spec)
{
    char args[512];
    int ret = 0;
    const AVFilter *buffersrc = NULL;
    const AVFilter *buffersink = NULL;
    AVFilterContext *buffersrc_ctx = NULL;
    AVFilterContext *buffersink_ctx = NULL;
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVFilterGraph *filter_graph = avfilter_graph_alloc();

    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        buffersrc = avfilter_get_by_name("buffer");
        buffersink = avfilter_get_by_name("buffersink");
        if (!buffersrc || !buffersink) {
            av_log(NULL, AV_LOG_ERROR, "filtering source or sink element not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        snprintf(args, sizeof(args),
                "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
                dec_ctx->time_base.num, dec_ctx->time_base.den,
                dec_ctx->sample_aspect_ratio.num,
                dec_ctx->sample_aspect_ratio.den);

        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                args, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
            goto end;
        }

        ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                NULL, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "pix_fmts",
                (uint8_t*)&enc_ctx->pix_fmt, sizeof(enc_ctx->pix_fmt),
                AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
            goto end;
        }
    } else {
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    /* Endpoints for the filter graph. */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if (!outputs->name || !inputs->name) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_spec,
                    &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;

    /* Fill FilteringContext */
    fctx->buffersrc_ctx = buffersrc_ctx;
    fctx->buffersink_ctx = buffersink_ctx;
    fctx->filter_graph = filter_graph;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

static int init_filters(struct thumbnail_ctx *ctx, unsigned int out_width, unsigned int out_height)
{
    char filter_spec[256];
    int ret;

    ctx->filter_ctx = av_malloc(sizeof(*ctx->filter_ctx));
    if (!ctx->filter_ctx)
        return AVERROR(ENOMEM);

    ctx->filter_ctx->buffersrc_ctx  = NULL;
    ctx->filter_ctx->buffersink_ctx = NULL;
    ctx->filter_ctx->filter_graph   = NULL;

    snprintf(filter_spec, sizeof(filter_spec), "scale=w=%d:h=%d", out_width, out_height);
    ret = init_filter(ctx, ctx->filter_ctx, ctx->stream_ctx->dec_ctx,
                      ctx->stream_ctx->enc_ctx, filter_spec);
    if (ret)
        return ret;

    return 0;
}

static int encode_write_frame(struct thumbnail_ctx *ctx,
                              AVFrame *filt_frame, unsigned int stream_index, int *got_frame) {
    int ret;
    int got_frame_local;
    AVPacket enc_pkt;

    if (!got_frame)
        got_frame = &got_frame_local;

    /* encode filtered frame */
    enc_pkt.data = NULL;
    enc_pkt.size = 0;
    av_init_packet(&enc_pkt);
    ret = avcodec_encode_video2(ctx->stream_ctx->enc_ctx, &enc_pkt,
                                filt_frame, got_frame);
    av_frame_free(&filt_frame);
    if (ret < 0)
        return ret;
    if (!(*got_frame))
        return 0;

    /* prepare packet for muxing */
    enc_pkt.stream_index = stream_index;
    av_packet_rescale_ts(&enc_pkt,
                         ctx->stream_ctx->enc_ctx->time_base,
                         ctx->ofmt_ctx->streams[stream_index]->time_base);

    /* mux encoded frame */
    ret = av_interleaved_write_frame(ctx->ofmt_ctx, &enc_pkt);
    return ret;
}

static int filter_encode_write_frame(struct thumbnail_ctx *ctx, AVFrame *frame, unsigned int stream_index)
{
    int ret;
    AVFrame *filt_frame;

    /* push the decoded frame into the filtergraph */
    ret = av_buffersrc_add_frame_flags(ctx->filter_ctx->buffersrc_ctx,
            frame, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
        return ret;
    }

    /* pull filtered frames from the filtergraph */
    filt_frame = av_frame_alloc();
    if (!filt_frame) {
        ret = AVERROR(ENOMEM);
        return ret;
    }

    ret = av_buffersink_get_frame(ctx->filter_ctx->buffersink_ctx,
                                  filt_frame);
    if (ret < 0) {
        /* if no more frames for output - returns AVERROR(EAGAIN)
         * if flushed and no more frames for output - returns AVERROR_EOF
         * rewrite retcode to 0 to show it as normal procedure completion
         */
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            ret = 0;
        av_frame_free(&filt_frame);
        return ret;
    }

    filt_frame->pict_type = AV_PICTURE_TYPE_NONE;
    ret = encode_write_frame(ctx, filt_frame, stream_index, NULL);

    return ret;
}

static int flush_encoder(struct thumbnail_ctx *ctx, unsigned int stream_index)
{
    int ret;
    int got_frame;

    if (!(ctx->stream_ctx->enc_ctx->codec->capabilities &
                AV_CODEC_CAP_DELAY))
        return 0;

    while (1) {
        av_log(NULL, AV_LOG_INFO, "Flushing stream #%u encoder\n", stream_index);
        ret = encode_write_frame(ctx, NULL, stream_index, &got_frame);
        if (ret < 0)
            break;
        if (!got_frame)
            return 0;
    }
    return ret;
}

int thumbnail_init(struct thumbnail_ctx *ctx, const char *out_filename,
                   unsigned int in_width, unsigned int in_height,
                   unsigned int out_width, unsigned int out_height,
                   unsigned int qscale)
{
    int ret;
    char *infilename;

    /* Note: Deprecated in 4.0 */
    av_register_all();
    avfilter_register_all();

    if ((ret = open_input_file(ctx, infilename, in_width, in_height)) < 0)
        goto end;

    if ((ret = open_output_file(ctx, out_filename, out_width, out_height, qscale)) < 0)
        goto end;

    if ((ret = init_filters(ctx, out_width, out_height)) < 0)
        goto end;

end:
    return 0;
}


int thumbnail_generate(struct thumbnail_ctx *ctx, AVPacket *packet)
{
    AVFrame *frame = NULL;
    unsigned int stream_index;
    int ret;
    int got_frame;

    stream_index = packet->stream_index;

    av_log(NULL, AV_LOG_DEBUG, "Going to reencode&filter the frame\n");
    frame = av_frame_alloc();
    if (!frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ret = avcodec_decode_video2(ctx->stream_ctx->dec_ctx, frame,
                                &got_frame, packet);
    if (ret < 0) {
        av_frame_free(&frame);
        av_log(NULL, AV_LOG_ERROR, "Decoding failed\n");
        goto end;
    }

    if (got_frame) {
        frame->pts = frame->best_effort_timestamp;
        ret = filter_encode_write_frame(ctx, frame, stream_index);
        av_frame_free(&frame);
        if (ret < 0)
            goto end;
    } else {
        av_frame_free(&frame);
    }

end:
    return ret ? 1 : 0;
}

int thumbnail_shutdown(struct thumbnail_ctx *ctx)
{
    unsigned int i;
    int ret;

    av_log(NULL, AV_LOG_DEBUG, "%s called...\n", __func__);

    /* flush filters and encoders */
    for (i = 0; i < 1; i++) {
        /* flush filter */
        if (!ctx->filter_ctx->filter_graph)
            continue;
        ret = filter_encode_write_frame(ctx, NULL, i);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Flushing filter failed\n");
            goto end;
        }

        /* flush encoder */
        ret = flush_encoder(ctx, i);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Flushing encoder failed\n");
            goto end;
        }
    }

    av_write_trailer(ctx->ofmt_ctx);
end:
//    av_packet_unref(&packet);
//    av_frame_free(&frame);
    avcodec_free_context(&ctx->stream_ctx->dec_ctx);
    if (ctx->ofmt_ctx && ctx->ofmt_ctx->nb_streams > i && ctx->ofmt_ctx->streams[i] && ctx->stream_ctx->enc_ctx)
        avcodec_free_context(&ctx->stream_ctx[i].enc_ctx);
    if (ctx->filter_ctx && ctx->filter_ctx->filter_graph)
        avfilter_graph_free(&ctx->filter_ctx->filter_graph);

    av_free(ctx->filter_ctx);
    av_free(ctx->stream_ctx);

    if (ctx->ofmt_ctx && !(ctx->ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&ctx->ofmt_ctx->pb);
    avformat_free_context(ctx->ofmt_ctx);

    if (ret < 0)
        av_log(NULL, AV_LOG_ERROR, "Error occurred: %s\n", av_err2str(ret));

    av_log(NULL, AV_LOG_ERROR, "%s done ret=%d\n", __func__, ret);

    return ret ? 1 : 0;
}

#define GET_PACKET_SIZE(w, h) (((w + 47) / 48) * 48 * h * 8 / 3)

int thumbnail_generate_buf(struct thumbnail_ctx *ctx, uint8_t *buf, unsigned int width, unsigned int height)
{
        AVPacket pkt;

        int ret;

        av_init_packet(&pkt);
        pkt.pts = ctx->frame_count++;
        pkt.dts = pkt.pts;
        pkt.duration = 0;
        pkt.flags       |= AV_PKT_FLAG_KEY;
        pkt.stream_index = 0;
        pkt.data         = buf;
        pkt.size         = GET_PACKET_SIZE(width, height);

        ret = thumbnail_generate(ctx, &pkt);
        av_packet_unref(&pkt);

        return ret;
}
