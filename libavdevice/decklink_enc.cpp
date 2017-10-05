/*
 * Blackmagic DeckLink output
 * Copyright (c) 2013-2014 Ramiro Polla
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

#include <atomic>
using std::atomic;

/* Include internal.h first to avoid conflict between winsock.h (used by
 * DeckLink headers) and winsock2.h (used by libavformat) in MSVC++ builds */
extern "C" {
#include "libavformat/internal.h"
}

#include <DeckLinkAPI.h>

extern "C" {
#include "libavformat/avformat.h"
#include "libavutil/imgutils.h"
}

#include "decklink_common.h"
#include "decklink_enc.h"
#include "libklscte35/scte35.h"
#include "libklvanc/vanc.h"
#include "libklvanc/vanc-lines.h"
#include "libklvanc/pixels.h"

/* DeckLink callback class declaration */
class decklink_frame : public IDeckLinkVideoFrame
{
public:
    decklink_frame(struct decklink_ctx *ctx, AVFrame *avframe, AVCodecID codec_id, int height, int width) :
        _ctx(ctx), _avframe(avframe), _avpacket(NULL), _codec_id(codec_id), _ancillary(NULL), _height(height), _width(width),  _refs(1) { }
    decklink_frame(struct decklink_ctx *ctx, AVPacket *avpacket, AVCodecID codec_id, int height, int width) :
        _ctx(ctx), _avframe(NULL), _avpacket(avpacket), _codec_id(codec_id), _ancillary(NULL), _height(height), _width(width), _refs(1) { }

    virtual long           STDMETHODCALLTYPE GetWidth      (void)          { return _width; }
    virtual long           STDMETHODCALLTYPE GetHeight     (void)          { return _height; }
    virtual long           STDMETHODCALLTYPE GetRowBytes   (void)
    {
      if (_codec_id == AV_CODEC_ID_WRAPPED_AVFRAME)
          return _avframe->linesize[0] < 0 ? -_avframe->linesize[0] : _avframe->linesize[0];
      else
          return ((GetWidth() + 47) / 48) * 128;
    }
    virtual BMDPixelFormat STDMETHODCALLTYPE GetPixelFormat(void)
    {
        if (_codec_id == AV_CODEC_ID_WRAPPED_AVFRAME)
            return bmdFormat8BitYUV;
        else
            return bmdFormat10BitYUV;
    }
    virtual BMDFrameFlags  STDMETHODCALLTYPE GetFlags      (void)
    {
       if (_codec_id == AV_CODEC_ID_WRAPPED_AVFRAME)
           return _avframe->linesize[0] < 0 ? bmdFrameFlagFlipVertical : bmdFrameFlagDefault;
       else
           return bmdFrameFlagDefault;
    }

    virtual HRESULT        STDMETHODCALLTYPE GetBytes      (void **buffer)
    {
      if (_avframe) {
        if (_avframe->linesize[0] < 0)
            *buffer = (void *)(_avframe->data[0] + _avframe->linesize[0] * (_avframe->height - 1));
        else
            *buffer = (void *)(_avframe->data[0]);
      } else {
            *buffer = (void *)(_avpacket->data);
      }
        return S_OK;
    }

    virtual HRESULT STDMETHODCALLTYPE GetTimecode     (BMDTimecodeFormat format, IDeckLinkTimecode **timecode) { return S_FALSE; }
    virtual HRESULT STDMETHODCALLTYPE GetAncillaryData(IDeckLinkVideoFrameAncillary **ancillary)
    {
        *ancillary = _ancillary;
        return _ancillary ? S_OK : S_FALSE;
    }
    virtual HRESULT STDMETHODCALLTYPE SetAncillaryData(IDeckLinkVideoFrameAncillary
                                                       *ancillary) { _ancillary = ancillary; return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) { return E_NOINTERFACE; }
    virtual ULONG   STDMETHODCALLTYPE AddRef(void)                            { return ++_refs; }
    virtual ULONG   STDMETHODCALLTYPE Release(void)
    {
        int ret = --_refs;
        if (!ret) {
            if (_codec_id == AV_CODEC_ID_WRAPPED_AVFRAME)
                av_frame_free(&_avframe);
            else
                av_packet_unref(_avpacket);
            delete this;
        }
        return ret;
    }

    struct decklink_ctx *_ctx;
    AVFrame *_avframe;
    AVPacket *_avpacket;
    AVCodecID _codec_id;
    IDeckLinkVideoFrameAncillary *_ancillary;
    int _height;
    int _width;

private:
    std::atomic<int>  _refs;
};

class decklink_output_callback : public IDeckLinkVideoOutputCallback
{
public:
    virtual HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame *_frame, BMDOutputFrameCompletionResult result)
    {
        decklink_frame *frame = static_cast<decklink_frame *>(_frame);
        struct decklink_ctx *ctx = frame->_ctx;
        AVFrame *avframe = frame->_avframe;

        if (avframe)
            av_frame_unref(avframe);
        else
            av_packet_unref(frame->_avpacket);

        pthread_mutex_lock(&ctx->mutex);
        ctx->frames_buffer_available_spots++;
        pthread_cond_broadcast(&ctx->cond);
        pthread_mutex_unlock(&ctx->mutex);

        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped(void)       { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) { return E_NOINTERFACE; }
    virtual ULONG   STDMETHODCALLTYPE AddRef(void)                            { return 1; }
    virtual ULONG   STDMETHODCALLTYPE Release(void)                           { return 1; }
};

static int decklink_setup_video(AVFormatContext *avctx, AVStream *st)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVCodecParameters *c = st->codecpar;

    if (ctx->video) {
        av_log(avctx, AV_LOG_ERROR, "Only one video stream is supported!\n");
        return -1;
    }

    if (c->codec_id == AV_CODEC_ID_WRAPPED_AVFRAME) {
        if (c->format != AV_PIX_FMT_UYVY422) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported pixel format!"
                   " Only AV_PIX_FMT_UYVY422 is supported.\n");
            return -1;
        }
    }

    if (ff_decklink_set_format(avctx, c->width, c->height,
                            st->time_base.num, st->time_base.den, c->field_order)) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported video size, framerate or field order!"
               " Check available formats with -list_formats 1.\n");
        return -1;
    }
    if (ctx->dlo->EnableVideoOutput(ctx->bmd_mode,
                                    bmdVideoOutputVANC) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not enable video output!\n");
        return -1;
    }

    avpacket_queue_init (avctx, &ctx->vanc_queue);

    /* Set callback. */
    ctx->output_callback = new decklink_output_callback();
    ctx->dlo->SetScheduledFrameCompletionCallback(ctx->output_callback);

    ctx->frames_preroll = st->time_base.den * ctx->preroll;
    if (st->time_base.den > 1000)
        ctx->frames_preroll /= 1000;

    /* Buffer twice as many frames as the preroll. */
    ctx->frames_buffer = ctx->frames_preroll * 2;
    ctx->frames_buffer = FFMIN(ctx->frames_buffer, 60);
    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->cond, NULL);
    ctx->frames_buffer_available_spots = ctx->frames_buffer;

    /* The device expects the framerate to be fixed. */
    avpriv_set_pts_info(st, 64, st->time_base.num, st->time_base.den);

    ctx->video = 1;

    return 0;
}

static int decklink_setup_audio(AVFormatContext *avctx, AVStream *st)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVCodecParameters *c = st->codecpar;

    if (ctx->audio) {
        av_log(avctx, AV_LOG_ERROR, "Only one audio stream is supported!\n");
        return -1;
    }
    if (c->sample_rate != 48000) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported sample rate!"
               " Only 48kHz is supported.\n");
        return -1;
    }
    if (c->channels != 2 && c->channels != 8) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported number of channels!"
               " Only stereo and 7.1 are supported.\n");
        return -1;
    }
    if (ctx->dlo->EnableAudioOutput(bmdAudioSampleRate48kHz,
                                    bmdAudioSampleType16bitInteger,
                                    c->channels,
                                    bmdAudioOutputStreamTimestamped) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not enable audio output!\n");
        return -1;
    }
    if (ctx->dlo->BeginAudioPreroll() != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not begin audio preroll!\n");
        return -1;
    }

    /* The device expects the sample rate to be fixed. */
    avpriv_set_pts_info(st, 64, 1, c->sample_rate);
    ctx->channels = c->channels;

    ctx->audio = 1;

    return 0;
}

av_cold int ff_decklink_write_trailer(AVFormatContext *avctx)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;

    if (ctx->playback_started) {
        BMDTimeValue actual;
        ctx->dlo->StopScheduledPlayback(ctx->last_pts * ctx->bmd_tb_num,
                                        &actual, ctx->bmd_tb_den);
        ctx->dlo->DisableVideoOutput();
        if (ctx->audio)
            ctx->dlo->DisableAudioOutput();
    }

    ff_decklink_cleanup(avctx);

    if (ctx->output_callback)
        delete ctx->output_callback;

    pthread_mutex_destroy(&ctx->mutex);
    pthread_cond_destroy(&ctx->cond);

    av_freep(&cctx->ctx);

    return 0;
}

static int decklink_write_video_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVStream *st = avctx->streams[pkt->stream_index];
    AVFrame *avframe = NULL, *tmp = (AVFrame *)pkt->data;
    AVPacket *avpacket = NULL;
    decklink_frame *frame;
    buffercount_type buffered;
    HRESULT hr;

    struct vanc_line_set_s vanc_lines;
    memset(&vanc_lines, 0, sizeof(vanc_lines));

    if (st->codecpar->codec_id == AV_CODEC_ID_WRAPPED_AVFRAME) {
        if (tmp->format != AV_PIX_FMT_UYVY422 ||
            tmp->width  != ctx->bmd_width ||
            tmp->height != ctx->bmd_height) {
            av_log(avctx, AV_LOG_ERROR, "Got a frame with invalid pixel format or dimension.\n");
            return AVERROR(EINVAL);
        }

        avframe = av_frame_clone(tmp);
        if (!avframe) {
            av_log(avctx, AV_LOG_ERROR, "Could not clone video frame.\n");
            return AVERROR(EIO);
        }

        frame = new decklink_frame(ctx, avframe, st->codecpar->codec_id, avframe->height, avframe->width);
    } else {
        avpacket = av_packet_clone(pkt);
        if (!avpacket) {
            av_log(avctx, AV_LOG_ERROR, "Could not clone video frame.\n");
            return AVERROR(EIO);
        }

        frame = new decklink_frame(ctx, avpacket, st->codecpar->codec_id, ctx->bmd_height, ctx->bmd_width);

        int size;
        const uint8_t *data = av_packet_get_side_data(pkt, AV_PKT_DATA_A53_CC, &size);
        if (data) {
            struct packet_eia_708b_s *pkt;
            uint16_t *cdp;
            uint16_t len;
            uint8_t cc_count = size / 3;

            klvanc_create_eia708_cdp(&pkt);
            klvanc_set_framerate_EIA_708B(pkt, ctx->bmd_tb_num, ctx->bmd_tb_den);

            /* CC data */
            pkt->header.ccdata_present = 1;
            pkt->ccdata.cc_count = cc_count;
            for (size_t i = 0; i < cc_count; i++) {
                if (data [3*i] & 0x40)
                    pkt->ccdata.cc[i].cc_valid = 1;
                pkt->ccdata.cc[i].cc_type = data[3*i] & 0x03;
                pkt->ccdata.cc[i].cc_data[0] = data[3*i+1];
                pkt->ccdata.cc[i].cc_data[1] = data[3*i+2];
            }

            klvanc_finalize_EIA_708B(pkt, ctx->cdp_sequence_num++);
            convert_EIA_708B_to_words(pkt, &cdp, &len);
            klvanc_destroy_eia708_cdp(pkt);

            vanc_line_insert(&vanc_lines, cdp, len, 11, 0);
        }

        /* See if there any pending data packets to process */
        if (avpacket_queue_size(&ctx->vanc_queue) > 0) {
            struct scte35_splice_info_section_s *s;
            AVStream *vanc_st;
            AVPacket vanc_pkt;
            int size;

            avpacket_queue_get(&ctx->vanc_queue, &vanc_pkt, 1);
            vanc_st = avctx->streams[vanc_pkt.stream_index];

            if (vanc_st->codecpar->codec_id == AV_CODEC_ID_SCTE_35) {
                const int64_t *orig_pts = (int64_t *) av_packet_get_side_data(&vanc_pkt, AV_PKT_DATA_ORIG_PTS, &size);
                s = scte35_splice_info_section_parse(vanc_pkt.data, vanc_pkt.size);
                if (s == NULL) {
                    fprintf(stderr, "Failed to splice section \n");
                    return 0;
                }

                /* Convert the SCTE35 message into a SCTE104 command */
                uint8_t *buf;
                uint16_t byteCount;
                int ret = scte35_create_scte104_message(s, &buf, &byteCount, orig_pts ? *orig_pts : 0);
                if (ret != 0) {
                    fprintf(stderr, "Unable to convert SCTE35 to SCTE104, ret = %d\n", ret);
                    scte35_splice_info_section_free(s);
                    return 0;
                }

                /* Generate a VANC line for SCTE104 message */
                uint16_t *vancWords = NULL;
                uint16_t vancWordCount;
                ret = vanc_sdi_create_payload(0x07, 0x41, buf, byteCount, &vancWords, &vancWordCount, 10);
                if (ret != 0) {
                    fprintf(stderr, "Error creating VANC message, ret = %d\n", ret);
                    return 0;
                }

                /* Free the allocated resource */
                scte35_splice_info_section_free(s);
                vanc_line_insert(&vanc_lines, vancWords, vancWordCount, 12, 0);
            } else if (vanc_st->codecpar->codec_id == AV_CODEC_ID_SMPTE_2038) {
                struct smpte2038_anc_data_packet_s *pkt_2038 = 0;

                smpte2038_parse_pes_payload(vanc_pkt.data, vanc_pkt.size, &pkt_2038);
                if (pkt_2038 == NULL) {
                    fprintf(stderr, "%s failed to decode PES packet\n", __func__);
                    return 0;
                }
                for (int i = 0; i < pkt_2038->lineCount; i++) {
                    struct smpte2038_anc_data_line_s *l = &pkt_2038->lines[i];
                    uint16_t *vancWords = NULL;
                    uint16_t vancWordCount;

                    if (smpte2038_convert_line_to_words(l, &vancWords, &vancWordCount) < 0)
                        break;

                    vanc_line_insert(&vanc_lines, vancWords, vancWordCount, l->line_number, 0);
                }
                smpte2038_anc_data_packet_free(pkt_2038);
            }
        }
    }


    IDeckLinkVideoFrameAncillary *vanc;
    int result = ctx->dlo->CreateAncillaryData(bmdFormat10BitYUV, &vanc);
    if (result != S_OK) {
        fprintf(stderr, "Failed to create vanc\n");
        return -1;
    }

    /* Now that we've got all the VANC lines in a nice orderly manner, generate the
       final VANC sections for the Decklink output */
    for (int i = 0; i < vanc_lines.num_lines; i++) {
        struct vanc_line_s *line = vanc_lines.lines[i];
        uint16_t *out_line;
        int real_line;
        int out_len;
        void *buf;

        if (line == NULL)
            break;

        real_line = line->line_number;
#if 0
        if (decklink_sys->b_psf_interlaced)
            real_line = Calculate1080psfVancLine(line->line_number);
#endif
        result = vanc->GetBufferForVerticalBlankingLine(real_line, &buf);
        if (result != S_OK) {
            fprintf(stderr, "Failed to get VANC line %d: %d", real_line, result);
            continue;
        }

        /* Generate the full line taking into account all VANC packets on that line */
        generate_vanc_line(line, &out_line, &out_len, ctx->bmd_width);

        /* Repack the 16-bit ints into 10-bit, and push into final buffer */
        klvanc_y10_to_v210(out_line, (uint8_t *) buf, out_len);
        free(out_line);
        vanc_line_free(line);
    }

    result = frame->SetAncillaryData(vanc);
    //vanc->Release(); /* FIXME: segfaults when uncommented */
    if (result != S_OK) {
        fprintf(stderr, "Failed to set vanc: %d", result);
        return AVERROR(EIO);
    }

    if (!frame) {
        av_log(avctx, AV_LOG_ERROR, "Could not create new frame.\n");

        if (avframe)
            av_frame_free(&avframe);
        else
            av_packet_unref(avpacket);
        return AVERROR(EIO);
    }

    /* Always keep at most one second of frames buffered. */
    pthread_mutex_lock(&ctx->mutex);
    while (ctx->frames_buffer_available_spots == 0) {
        pthread_cond_wait(&ctx->cond, &ctx->mutex);
    }
    ctx->frames_buffer_available_spots--;
    pthread_mutex_unlock(&ctx->mutex);

    /* Schedule frame for playback. */
    hr = ctx->dlo->ScheduleVideoFrame((struct IDeckLinkVideoFrame *) frame,
                                      pkt->pts * ctx->bmd_tb_num,
                                      ctx->bmd_tb_num, ctx->bmd_tb_den);
    /* Pass ownership to DeckLink, or release on failure */
    frame->Release();
    if (hr != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not schedule video frame."
                " error %08x.\n", (uint32_t) hr);
        return AVERROR(EIO);
    }

    ctx->dlo->GetBufferedVideoFrameCount(&buffered);
    av_log(avctx, AV_LOG_DEBUG, "Buffered video frames: %d.\n", (int) buffered);
    if (pkt->pts > 2 && buffered <= 2)
        av_log(avctx, AV_LOG_WARNING, "There are not enough buffered video frames."
               " Video may misbehave!\n");

    /* Preroll video frames. */
    if (!ctx->playback_started && pkt->pts > ctx->frames_preroll) {
        av_log(avctx, AV_LOG_DEBUG, "Ending audio preroll.\n");
        if (ctx->audio && ctx->dlo->EndAudioPreroll() != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Could not end audio preroll!\n");
            return AVERROR(EIO);
        }
        av_log(avctx, AV_LOG_DEBUG, "Starting scheduled playback.\n");
        if (ctx->dlo->StartScheduledPlayback(0, ctx->bmd_tb_den, 1.0) != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Could not start scheduled playback!\n");
            return AVERROR(EIO);
        }
        ctx->playback_started = 1;
    }

    return 0;
}

static int decklink_write_audio_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    int sample_count = pkt->size / (ctx->channels << 1);
    buffercount_type buffered;

    ctx->dlo->GetBufferedAudioSampleFrameCount(&buffered);
    if (pkt->pts > 1 && !buffered)
        av_log(avctx, AV_LOG_WARNING, "There's no buffered audio."
               " Audio will misbehave!\n");

    if (ctx->dlo->ScheduleAudioSamples(pkt->data, sample_count, pkt->pts,
                                       bmdAudioSampleRate48kHz, NULL) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not schedule audio samples.\n");
        return AVERROR(EIO);
    }

    return 0;
}

static int decklink_write_data_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;

    AVPacket *avpacket = av_packet_clone(pkt);
    if (avpacket_queue_put(&ctx->vanc_queue, avpacket) < 0) {
        fprintf(stderr, "Failed to queue VANC packet\n");
    }

    return 0;
}

extern "C" {

av_cold int ff_decklink_write_header(AVFormatContext *avctx)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx;
    unsigned int n;
    int ret;

    ctx = (struct decklink_ctx *) av_mallocz(sizeof(struct decklink_ctx));
    if (!ctx)
        return AVERROR(ENOMEM);
    ctx->list_devices = cctx->list_devices;
    ctx->list_formats = cctx->list_formats;
    ctx->preroll      = cctx->preroll;
    cctx->ctx = ctx;

    /* List available devices. */
    if (ctx->list_devices) {
        ff_decklink_list_devices(avctx);
        return AVERROR_EXIT;
    }

    ret = ff_decklink_init_device(avctx, avctx->filename);
    if (ret < 0)
        return ret;

    /* Get output device. */
    if (ctx->dl->QueryInterface(IID_IDeckLinkOutput, (void **) &ctx->dlo) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not open output device from '%s'\n",
               avctx->filename);
        ret = AVERROR(EIO);
        goto error;
    }

    /* List supported formats. */
    if (ctx->list_formats) {
        ff_decklink_list_formats(avctx);
        ret = AVERROR_EXIT;
        goto error;
    }

    /* Setup streams. */
    ret = AVERROR(EIO);
    for (n = 0; n < avctx->nb_streams; n++) {
        AVStream *st = avctx->streams[n];
        AVCodecParameters *c = st->codecpar;
        if        (c->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (decklink_setup_audio(avctx, st))
                goto error;
        } else if (c->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (decklink_setup_video(avctx, st))
                goto error;
        } else if (c->codec_type == AVMEDIA_TYPE_DATA) {
            /* No actual setup required */
        } else {
            av_log(avctx, AV_LOG_ERROR, "Unsupported stream type.\n");
            goto error;
        }
    }

    return 0;

error:
    ff_decklink_cleanup(avctx);
    return ret;
}

int ff_decklink_write_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVStream *st = avctx->streams[pkt->stream_index];

    ctx->last_pts = FFMAX(ctx->last_pts, pkt->pts);

    if      (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        return decklink_write_video_packet(avctx, pkt);
    else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        return decklink_write_audio_packet(avctx, pkt);
    else if (st->codecpar->codec_type == AVMEDIA_TYPE_DATA) {
        return decklink_write_data_packet(avctx, pkt);
    }

    return AVERROR(EIO);
}

} /* extern "C" */
