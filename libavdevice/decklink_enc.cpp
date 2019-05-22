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

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include <atomic>
using std::atomic;

/* Include internal.h first to avoid conflict between winsock.h (used by
 * DeckLink headers) and winsock2.h (used by libavformat) in MSVC++ builds */
extern "C" {
#include "libavformat/internal.h"
}

#include <DeckLinkAPI.h>
#include <DeckLinkAPIVersion.h>

extern "C" {
#include "libavformat/avformat.h"
#include "libavformat/network.h"
#include "libavformat/os_support.h"
#include "libavutil/imgutils.h"
#include "libavutil/avstring.h"
#include "libavutil/vtune.h"
#include "avdevice.h"
}

#include "decklink_common.h"
#include "decklink_enc.h"
#if CONFIG_LIBKLVANC
#include "libklvanc/vanc.h"
#include "libklvanc/vanc-lines.h"
#include "libklvanc/pixels.h"
#endif

/* If the PTS of the latest audio packet is within this number of the previous
   packet received, just concatenate the blocks.  This is to deal with certain
   encoders which provide PTS values that are slightly off from the actual number
   of samples delivered.  Without this, we either introduce small gaps between
   the audio blocks, or we end up overwriting the last few samples of the previous
   audio block.  */
#define AUDIO_PTS_FUDGEFACTOR 15

static void udp_monitor_report(int fd, const char *str, uint64_t val)
{
    char *buf;

    if (fd < 0)
        return;

    buf = av_asprintf("%s: %" PRId64 "\n", str, val);
    if (!buf)
        return;

    send(fd, buf, strlen(buf), 0);
    av_free(buf);
}


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
        if (_codec_id == AV_CODEC_ID_WRAPPED_AVFRAME) {
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
            av_frame_free(&_avframe);
            av_packet_free(&_avpacket);
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

class decklink_output_callback : public IDeckLinkVideoOutputCallback, public IDeckLinkAudioOutputCallback
{
public:
    struct decklink_cctx *_cctx;
    AVFormatContext *_avctx;
    decklink_output_callback(struct decklink_cctx *cctx, AVFormatContext *avctx)
    {
        _cctx = cctx;
        _avctx = avctx;
    }
    virtual HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame *_frame, BMDOutputFrameCompletionResult result)
    {
        decklink_frame *frame = static_cast<decklink_frame *>(_frame);
        struct decklink_ctx *ctx = frame->_ctx;

        if (frame->_avframe)
            av_frame_unref(frame->_avframe);
        if (frame->_avpacket)
            av_packet_unref(frame->_avpacket);

        pthread_mutex_lock(&ctx->mutex);
        ctx->frames_buffer_available_spots++;
        pthread_cond_broadcast(&ctx->cond);
        pthread_mutex_unlock(&ctx->mutex);

        switch (result) {
        case bmdOutputFrameCompleted:
        case bmdOutputFrameFlushed:
            break;
        case bmdOutputFrameDisplayedLate:
            ctx->late++;
            av_log(_avctx, AV_LOG_WARNING, "Video buffer late\n");
            av_vtune_log_stat(DECKLINK_BUFFERS_LATE, ctx->late, 0);
            break;
        case bmdOutputFrameDropped:
            ctx->dropped++;
            av_log(_avctx, AV_LOG_WARNING, "Video buffer dropped\n");
            av_vtune_log_stat(DECKLINK_BUFFERS_DROPPED, ctx->dropped, 0);
            break;
        }

        av_vtune_log_stat(DECKLINK_BUFFER_COUNT, ctx->frames_buffer_available_spots, 0);

        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped(void)       { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE RenderAudioSamples (bool preroll)
    {
        struct decklink_ctx *ctx = (struct decklink_ctx *)_cctx->ctx;
        AVPacketList *cur, *next;
        BMDTimeValue streamtime;
        double speed;
        ctx->dlo->GetScheduledStreamTime(48000, &streamtime, &speed);

        pthread_mutex_lock(&ctx->audio_mutex);

        /* Do final Scheduling of audio at least 0.25 seconds before realtime.  This might
           need to be configurable at some point if the user wants to do really low
           latency (i.e. a pre-roll of less than 0.25 seconds)...  */
        int64_t window = streamtime + 12000;
        if (window > 0) {
            for (cur = ctx->output_audio_list; cur != NULL; cur = next) {
                if (cur->next == NULL)
                    break;
                if (cur->pkt.pts > window)
                    break;
                uint32_t written;
                HRESULT result = ctx->dlo->ScheduleAudioSamples(cur->pkt.data,
                                                                ctx->audio_pkt_numsamples, cur->pkt.pts,
                                                                bmdAudioSampleRate48kHz,
                                                                &written);
                if (result != S_OK)
                    udp_monitor_report(ctx->udp_fd, "ERROR AUDIO", result);
                else
                    udp_monitor_report(ctx->udp_fd, "PLAY AUDIO BYTES", written);

                ctx->output_audio_list = cur->next;
                next = cur->next;
                av_packet_unref(&cur->pkt);
                av_freep(&cur);
            }
        }

        pthread_mutex_unlock(&ctx->audio_mutex);

        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) { return E_NOINTERFACE; }
    virtual ULONG   STDMETHODCALLTYPE AddRef(void)                            { return 1; }
    virtual ULONG   STDMETHODCALLTYPE Release(void)                           { return 1; }
};

#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0a080000
static void reset_output(AVFormatContext *avctx, IDeckLink *p_card, IDeckLinkOutput *p_output)
{
    /* The decklink driver can sometimes get stuck in a state where
       EnableVideoOutput always fails.  To work around this issue,
       call it with the last configured output mode, which causes it
       to recover */
    IDeckLinkStatus *p_status;
    int64_t vid_mode;
    int result;

    result = p_card->QueryInterface(IID_IDeckLinkStatus, (void**)&p_status);
    if (result != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not get status interface");
        return;
    }

    result = p_status->GetInt(bmdDeckLinkStatusCurrentVideoOutputMode, &vid_mode);
    if (result != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not get current video output mode");
        p_status->Release();
        return;
    }

    result = p_output->EnableVideoOutput(vid_mode, bmdVideoOutputFlagDefault);
    if (result != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not get enable video output mode");
        p_status->Release();
        return;
    }

    p_output->DisableVideoOutput();
    p_status->Release();
}
#endif

#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0a040000
static int setup_3g_level_a(AVFormatContext *avctx)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    DECKLINK_BOOL supports_level_a = false;
    DECKLINK_BOOL level_a = false;
    DECKLINK_BOOL desired_level_a = false;
    HRESULT res;

    if (cctx->use_3glevel_a == -1) {
        /* The user didn't specify which mode they wanted to use, so output
           in whatever format the system was already configured for */
        return 0;
    }

    if (cctx->use_3glevel_a)
        desired_level_a = true;
    else
        desired_level_a = false;

    /* First check to see if the card supports 3G-SDI Level A */
    res = ctx->attr->GetFlag(BMDDeckLinkSupportsSMPTELevelAOutput, &supports_level_a);
    if (res != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to determine if card supports Level A");
        return -1;
    }

    if (desired_level_a && !supports_level_a) {
        av_log(avctx, AV_LOG_ERROR, "User requested 3G Level A but not supported by card");
        return -1;
    }

    /* Grab the current value so we only change it if needed */
    res = ctx->cfg->GetFlag(bmdDeckLinkConfigSMPTELevelAOutput, &level_a);
    if (res != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get current status of 3G Level A\n");
        return -1;
    }

    if (level_a != desired_level_a) {
        av_log(avctx, AV_LOG_INFO, "Need to %s 3G-Level A\n",
               desired_level_a ? "Enable" : "Disable");
        res = ctx->cfg->SetFlag(bmdDeckLinkConfigSMPTELevelAOutput, desired_level_a);
        if (res != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to select set output to Level A\n");
            return -1;
        }
        res = ctx->cfg->WriteConfigurationToPreferences();
        if (res != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed writing updated configuration\n");
            return -1;
        }
    }
    return 0;
}
#endif

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
        ctx->raw_format = bmdFormat8BitYUV;
    } else if (c->codec_id != AV_CODEC_ID_V210) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported codec type!"
               " Only V210 and wrapped frame with AV_PIX_FMT_UYVY422 are supported.\n");
        return -1;
    } else {
        ctx->raw_format = bmdFormat10BitYUV;
    }

    if (ff_decklink_set_configs(avctx, DIRECTION_OUT) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Could not set output configuration\n");
        return -1;
    }
    if (ff_decklink_set_format(avctx, c->width, c->height,
                            st->time_base.num, st->time_base.den, c->field_order)) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported video size, framerate or field order!"
               " Check available formats with -list_formats 1.\n");
        return -1;
    }

#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0a080000
    reset_output(avctx, ctx->dl, ctx->dlo);
#endif

#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0a040000
    if (ctx->bmd_mode == bmdModeHD1080p50 ||
        ctx->bmd_mode == bmdModeHD1080p5994 ||
        ctx->bmd_mode == bmdModeHD1080p6000) {
        setup_3g_level_a(avctx);
    }
#endif

    if (ctx->dlo->EnableVideoOutput(ctx->bmd_mode,
                                    ctx->supports_vanc ? bmdVideoOutputVANC : bmdVideoOutputFlagDefault) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not enable video output!\n");
        return -1;
    }

    avpacket_queue_init (avctx, &ctx->vanc_queue);

    /* Set callback. */
    ctx->output_callback = new decklink_output_callback(cctx, avctx);
    ctx->dlo->SetScheduledFrameCompletionCallback(ctx->output_callback);
    ctx->dlo->SetAudioCallback(ctx->output_callback);

    ctx->frames_preroll = st->time_base.den * ctx->preroll;
    if (st->time_base.den > 1000)
        ctx->frames_preroll /= 1000;

    /* Buffer twice as many frames as the preroll. */
    ctx->frames_buffer = ctx->frames_preroll * 2;
    ctx->frames_buffer = FFMIN(ctx->frames_buffer, 60);

    /* Throw the first X frames so that all upstream FIFOs have the opportunity
       to flush (to reduce realtime latency) */
    ctx->frames_discard = st->time_base.den * cctx->discard / st->time_base.num;

    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_mutex_init(&ctx->audio_mutex, NULL);
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

    if (st->codecpar->codec_id == AV_CODEC_ID_AC3) {
        /* Regardless of the number of channels in the codec, we're only
           using 2 SDI audio channels at 48000Hz */
        ctx->channels += 2;
    } else if (st->codecpar->codec_id == AV_CODEC_ID_PCM_S16LE) {
        if (c->sample_rate != bmdAudioSampleRate48kHz) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported sample rate!"
                   " Only 48kHz is supported.\n");
            return -1;
        }
        ctx->channels += c->channels;
    } else {
        av_log(avctx, AV_LOG_ERROR, "Unsupported codec specified!"
               " Only PCM_S16LE and AC-3 are supported.\n");
        return -1;
    }

    /* The device expects the sample rate to be fixed. */
    avpriv_set_pts_info(st, 64, 1, bmdAudioSampleRate48kHz);

    ctx->audio++;

    return 0;
}

static int decklink_enable_audio(AVFormatContext *avctx)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;

    /* Round up total channel count to that supported by decklink.  This
       means we may need to pad the output buffer when interleaving the
       audio packet data... */
    if (ctx->channels <= 2)
        ctx->channels = 2;
    else if (ctx->channels <= 8)
        ctx->channels = 8;
    else if (ctx->channels <= 16)
        ctx->channels = 16;

    if (ctx->dlo->EnableAudioOutput(bmdAudioSampleRate48kHz,
                                    bmdAudioSampleType16bitInteger,
                                    ctx->channels,
                                    bmdAudioOutputStreamTimestamped) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not enable audio output!\n");
        return -1;
    }
    if (ctx->dlo->BeginAudioPreroll() != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not begin audio preroll!\n");
        return -1;
    }

    return 0;
}

static int decklink_setup_data(AVFormatContext *avctx, AVStream *st)
{
    int ret = -1;

    switch(st->codecpar->codec_id) {
#if CONFIG_LIBKLVANC
    case AV_CODEC_ID_SMPTE_2038:
    case AV_CODEC_ID_SCTE_104:
        /* No specific setup required */
        ret = 0;
        break;
    case AV_CODEC_ID_SCTE_35:
#if CONFIG_SCTE35TOSCTE104_BSF
        if (ff_stream_add_bitstream_filter(st, "scte35toscte104", NULL) > 0) {
            st->codecpar->codec_id = AV_CODEC_ID_SCTE_104;
            ret = 0;
        }
#else
        av_log(avctx, AV_LOG_ERROR, "SCTE-35 requires scte35toscte104 BSF to be available\n");
#endif
        break;
#endif
    default:
        av_log(avctx, AV_LOG_ERROR, "Unsupported data codec specified\n");
    }
    return ret;
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

    if (ctx->udp_fd >= 0)
        closesocket(ctx->udp_fd);

    av_freep(&cctx->ctx);

    return 0;
}

#if CONFIG_LIBKLVANC
static void construct_cc(AVFormatContext *avctx, struct decklink_cctx *cctx,
                         AVPacket *pkt, decklink_frame *frame,
                         AVStream *st, struct klvanc_line_set_s *vanc_lines)
{
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    const uint8_t *data;
    int ret, size;

    data = av_packet_get_side_data(pkt, AV_PKT_DATA_A53_CC, &size);
    if (data && cctx->cea708_line != -1) {
        struct klvanc_packet_eia_708b_s *pkt;
        uint16_t *cdp;
        uint16_t len;
        uint8_t cc_count = size / 3;

        ret = klvanc_create_eia708_cdp(&pkt);
        if (ret != 0)
            return;

        ret = klvanc_set_framerate_EIA_708B(pkt, ctx->bmd_tb_num, ctx->bmd_tb_den);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Invalid framerate specified: %lld/%lld\n",
                   ctx->bmd_tb_num, ctx->bmd_tb_den);
            klvanc_destroy_eia708_cdp(pkt);
            return;
        }

        if (cc_count > KLVANC_MAX_CC_COUNT) {
            av_log(avctx, AV_LOG_ERROR, "Illegal cc_count received: %d\n", cc_count);
            cc_count = KLVANC_MAX_CC_COUNT;
        }

        /* CC data */
        pkt->header.ccdata_present = 1;
        pkt->header.caption_service_active = 1;
        pkt->ccdata.cc_count = cc_count;
        for (size_t i = 0; i < cc_count; i++) {
            if (data [3*i] & 0x04)
                pkt->ccdata.cc[i].cc_valid = 1;
            pkt->ccdata.cc[i].cc_type = data[3*i] & 0x03;
            pkt->ccdata.cc[i].cc_data[0] = data[3*i+1];
            pkt->ccdata.cc[i].cc_data[1] = data[3*i+2];
        }

        klvanc_finalize_EIA_708B(pkt, ctx->cdp_sequence_num++);
        ret = klvanc_convert_EIA_708B_to_words(pkt, &cdp, &len);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed converting 708 packet to words\n");
            return;
        }
        klvanc_destroy_eia708_cdp(pkt);

        ret = klvanc_line_insert(ctx->vanc_ctx, vanc_lines, cdp, len,
                                 cctx->cea708_line, 0);
        free(cdp);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "VANC line insertion failed\n");
            return;
        }
        udp_monitor_report(ctx->udp_fd, "CC COUNT", cc_count);
    }
}

static void construct_afd(AVFormatContext *avctx, struct decklink_cctx *cctx,
                          AVPacket *pkt, decklink_frame *frame,
                          AVStream *st, struct klvanc_line_set_s *vanc_lines)
{
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVBarData *bardata;
    int ret, size, bardata_size;
    const uint8_t *data;

    data = av_packet_get_side_data(pkt, AV_PKT_DATA_AFD, &size);
    bardata = (AVBarData *) av_packet_get_side_data(pkt, AV_PKT_DATA_BARDATA, &bardata_size);
    if ((data || bardata) && cctx->afd_line != -1) {
        struct klvanc_packet_afd_s *pkt;
        uint16_t *afd;
        uint16_t len;

        ret = klvanc_create_AFD(&pkt);
        if (ret != 0)
            return;

        if (data) {
            ret = klvanc_set_AFD_val(pkt, data[0]);
            if (ret != 0) {
                av_log(avctx, AV_LOG_ERROR, "Invalid AFD value specified: %d\n",
                       data[0]);
                klvanc_destroy_AFD(pkt);
                return;
            }
        }

        if (bardata) {
            if (bardata->top_bottom) {
                pkt->barDataFlags = BARS_TOPBOTTOM;
                pkt->top = bardata->top;
                pkt->bottom = bardata->bottom;
            } else {
                pkt->barDataFlags = BARS_LEFTRIGHT;
                pkt->left = bardata->left;
                pkt->right = bardata->right;
            }
        }

        /* FIXME: Should really rely on the coded_width but seems like that
           is not accessible to libavdevice outputs */
        if ((st->codecpar->width == 1280 && st->codecpar->height == 720) ||
            (st->codecpar->width == 1920 && st->codecpar->height == 1080))
            pkt->aspectRatio = ASPECT_16x9;
        else
            pkt->aspectRatio = ASPECT_4x3;

        ret = klvanc_convert_AFD_to_words(pkt, &afd, &len);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed converting AFD packet to words\n");
            return;
        }
        klvanc_destroy_AFD(pkt);

        ret = klvanc_line_insert(ctx->vanc_ctx, vanc_lines, afd, len,
                                 cctx->afd_line, 0);
        free(afd);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "VANC line insertion failed\n");
            return;
        }
    }
}


static int decklink_construct_vanc(AVFormatContext *avctx, struct decklink_cctx *cctx,
                                   AVPacket *pkt, decklink_frame *frame,
                                   AVStream *st)
{
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    struct klvanc_line_set_s vanc_lines = { 0 };
    int ret;

    if (ctx->supports_vanc == 0)
        return 0;

    construct_cc(avctx, cctx, pkt, frame, st, &vanc_lines);
    construct_afd(avctx, cctx, pkt, frame, st, &vanc_lines);

    /* See if there any pending data packets to process */
    int dequeue_size = avpacket_queue_size(&ctx->vanc_queue);
    while (dequeue_size > 0) {
        AVStream *vanc_st;
        AVPacket vanc_pkt;

        avpacket_queue_get(&ctx->vanc_queue, &vanc_pkt, 1);
        dequeue_size -= (vanc_pkt.size + sizeof(AVPacketList));
        if (vanc_pkt.pts + 1 < ctx->last_pts) {
            av_log(avctx, AV_LOG_WARNING, "VANC packet too old, throwing away\n");
            av_packet_unref(&vanc_pkt);
            continue;
        }

        vanc_st = avctx->streams[vanc_pkt.stream_index];

        if (vanc_st->codecpar->codec_id == AV_CODEC_ID_SMPTE_2038) {
            struct klvanc_smpte2038_anc_data_packet_s *pkt_2038 = 0;

            klvanc_smpte2038_parse_pes_payload(vanc_pkt.data, vanc_pkt.size, &pkt_2038);
            if (pkt_2038 == NULL) {
                av_log(avctx, AV_LOG_ERROR, "failed to decode SMPTE 2038 PES packet");
                av_packet_unref(&vanc_pkt);
                continue;
            }
            for (int i = 0; i < pkt_2038->lineCount; i++) {
                struct klvanc_smpte2038_anc_data_line_s *l = &pkt_2038->lines[i];
                uint16_t *vancWords = NULL;
                uint16_t vancWordCount;

                if (klvanc_smpte2038_convert_line_to_words(l, &vancWords,
                                                           &vancWordCount) < 0)
                    break;

                ret = klvanc_line_insert(ctx->vanc_ctx, &vanc_lines, vancWords,
                                         vancWordCount, l->line_number, 0);
                free(vancWords);
                if (ret != 0) {
                    av_log(avctx, AV_LOG_ERROR, "VANC line insertion failed\n");
                    break;
                }
            }
            klvanc_smpte2038_anc_data_packet_free(pkt_2038);
        } else if (vanc_st->codecpar->codec_id == AV_CODEC_ID_SCTE_104) {
            if (cctx->scte104_line == -1) {
                av_packet_unref(&vanc_pkt);
                continue;
            }

	    /* SCTE-104 packets cannot be directly embedded into SDI.  They needs to
               be encapsulated in SMPTE 2010 first) */
            uint8_t *smpte2010_bytes;
            uint16_t smpte2010_len;
            ret = klvanc_convert_SCTE_104_packetbytes_to_SMPTE_2010(ctx->vanc_ctx,
                                                                    vanc_pkt.data,
                                                                    vanc_pkt.size,
                                                                    &smpte2010_bytes,
                                                                    &smpte2010_len);
            if (ret != 0) {
                av_log(avctx, AV_LOG_ERROR, "Error creating SMPTE 2010 VANC payload, ret=%d\n",
                       ret);
                break;
            }

            /* Generate a VANC line for SCTE104 message */
            uint16_t *vancWords = NULL;
            uint16_t vancWordCount;
            ret = klvanc_sdi_create_payload(0x07, 0x41, smpte2010_bytes, smpte2010_len,
                                            &vancWords, &vancWordCount, 10);
            free(smpte2010_bytes);
            if (ret != 0) {
                av_log(avctx, AV_LOG_ERROR, "Error creating SCTE-104 VANC payload, ret=%d\n",
                       ret);
                break;
            }
            ret = klvanc_line_insert(ctx->vanc_ctx, &vanc_lines, vancWords,
                                     vancWordCount, cctx->scte104_line, 0);
            free(vancWords);
            if (ret != 0) {
                av_log(avctx, AV_LOG_ERROR, "VANC line insertion failed\n");
                break;
            }
        }
        av_packet_unref(&vanc_pkt);
    }

    IDeckLinkVideoFrameAncillary *vanc;
    int result = ctx->dlo->CreateAncillaryData(bmdFormat10BitYUV, &vanc);
    if (result != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create vanc\n");
        return -1;
    }

    /* Now that we've got all the VANC lines in a nice orderly manner, generate the
       final VANC sections for the Decklink output */
    for (int i = 0; i < vanc_lines.num_lines; i++) {
        struct klvanc_line_s *line = vanc_lines.lines[i];
        int real_line;
        void *buf;

        if (line == NULL)
            break;

        real_line = line->line_number;
#if 0
        /* FIXME: include hack for certain Decklink cards which mis-represent
           line numbers for pSF frames */
        if (decklink_sys->b_psf_interlaced)
            real_line = Calculate1080psfVancLine(line->line_number);
#endif
        result = vanc->GetBufferForVerticalBlankingLine(real_line, &buf);
        if (result != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get VANC line %d: %d", real_line, result);
            klvanc_line_free(line);
            continue;
        }

        /* Generate the full line taking into account all VANC packets on that line */
        result = klvanc_generate_vanc_line_v210(ctx->vanc_ctx, line, (uint8_t *) buf,
                                                ctx->bmd_width);
        if (result != 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to generate VANC line\n");
            klvanc_line_free(line);
            continue;
        }

        klvanc_line_free(line);
    }

    result = frame->SetAncillaryData(vanc);
    if (result != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set vanc: %d", result);
        return AVERROR(EIO);
    }
    return 0;
}
#endif

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
    uint64_t t1;
#if CONFIG_LIBKLVANC
    int ret;
#endif

    t1 = av_vtune_get_timestamp();
    ctx->last_pts = FFMAX(ctx->last_pts, pkt->pts);

    BMDTimeValue streamtime;
    int64_t delta;
    ctx->dlo->GetScheduledStreamTime(ctx->bmd_tb_den, &streamtime, NULL);
    delta = pkt->pts - (streamtime / ctx->bmd_tb_num);
    av_vtune_log_stat(DECKLINK_QUEUE_DELTA, delta, 0);

    if (ctx->frames_discard-- > 0) {
        av_log(avctx, AV_LOG_ERROR, "Discarding frame with PTS %" PRId64 " discard=%d\n",
               pkt->pts, ctx->frames_discard);
        av_frame_free(&avframe);
        av_packet_free(&avpacket);
        return 0;
    }

#if 0
    av_log(avctx, AV_LOG_INFO,
           "started=%d streamtime=%ld delta=%ld first=%ld fb=%d\n",
           ctx->playback_started, streamtime, delta, ctx->first_pts,
           ctx->frames_buffer);
#endif
    if (ctx->playback_started && (delta < 0 || delta > ctx->frames_buffer)) {
        /* We're behind realtime, or way too far ahead, so restart clocks */
        av_log(avctx, AV_LOG_ERROR, "Scheduled frames received too %s.  "
               "Restarting output.  Delta=%" PRId64 "\n", delta < 0 ? "late" : "far into future", delta);
        if (ctx->dlo->StopScheduledPlayback(0, NULL, 0) != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to stop scheduled playback\n");
            return AVERROR(EIO);
        }

        ctx->frames_discard = st->time_base.den * cctx->discard / st->time_base.num;
        ctx->first_pts = pkt->pts + ctx->frames_discard;
        ctx->playback_started = 0;
        if (ctx->audio && ctx->dlo->BeginAudioPreroll() != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Could not begin audio preroll!\n");
            return -1;
        }
    }

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

#if CONFIG_LIBKLVANC
        ret = decklink_construct_vanc(avctx, cctx, pkt, frame, st);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to construct VANC\n");
        }
#endif
    }

    if (!frame) {
        av_log(avctx, AV_LOG_ERROR, "Could not create new frame.\n");
        av_frame_free(&avframe);
        av_packet_free(&avpacket);
        return AVERROR(EIO);
    }

    /* Always keep at most one second of frames buffered. */
    av_vtune_log_stat(DECKLINK_BUFFER_COUNT, ctx->frames_buffer_available_spots, 0);

    pthread_mutex_lock(&ctx->mutex);
    while (ctx->frames_buffer_available_spots == 0) {
        pthread_cond_wait(&ctx->cond, &ctx->mutex);
    }
    ctx->frames_buffer_available_spots--;
    pthread_mutex_unlock(&ctx->mutex);

    if (ctx->first_pts == 0)
        ctx->first_pts = pkt->pts;

    /* Schedule frame for playback. */
    hr = ctx->dlo->ScheduleVideoFrame((class IDeckLinkVideoFrame *) frame,
                                      pkt->pts * ctx->bmd_tb_num,
                                      ctx->bmd_tb_num, ctx->bmd_tb_den);
    /* Pass ownership to DeckLink, or release on failure */
    frame->Release();
    if (hr != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not schedule video frame."
                " error %08x.\n", (uint32_t) hr);
        return AVERROR(EIO);
    }

    udp_monitor_report(ctx->udp_fd, "PICTURE", pkt->pts);

    ctx->dlo->GetBufferedVideoFrameCount(&buffered);
    av_log(avctx, AV_LOG_DEBUG, "Buffered video frames: %d.\n", (int) buffered);
    if (pkt->pts > 2 && buffered <= 2)
        av_log(avctx, AV_LOG_WARNING, "There are not enough buffered video frames."
               " Video may misbehave!\n");

    /* Preroll video frames. */
    if (!ctx->playback_started && pkt->pts >= (ctx->first_pts + ctx->frames_preroll - 1)) {
        if (ctx->audio && ctx->dlo->EndAudioPreroll() != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Could not end audio preroll!\n");
            return AVERROR(EIO);
        }
        av_log(avctx, AV_LOG_DEBUG, "Starting scheduled playback.\n");
        if (ctx->dlo->StartScheduledPlayback(ctx->first_pts * ctx->bmd_tb_num, ctx->bmd_tb_den, 1.0) != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Could not start scheduled playback!\n");
            return AVERROR(EIO);
        }
        ctx->playback_started = 1;
    }

    av_vtune_log_event("write_video", t1, av_vtune_get_timestamp(), 1);

    return 0;
}

static int create_s337_payload(AVPacket *pkt, enum AVCodecID codec_id, uint8_t **outbuf, int *outsize)
{
    uint8_t *s337_payload;
    uint8_t *s337_payload_start;
    int i;

    /* Encapsulate AC3 syncframe into SMPTE 337 packet */
    *outsize = pkt->size + 8;
    s337_payload = (uint8_t *) av_mallocz(*outsize);
    if (s337_payload == NULL)
        return AVERROR(ENOMEM);

    *outbuf = s337_payload;

    /* Construct SMPTE S337 Burst preamble */
    s337_payload[0] = 0x72; /* Sync Word 1 */
    s337_payload[1] = 0xf8; /* Sync Word 1 */
    s337_payload[2] = 0x1f; /* Sync Word 1 */
    s337_payload[3] = 0x4e; /* Sync Word 1 */

    if (codec_id == AV_CODEC_ID_AC3) {
        s337_payload[4] = 0x01;
    } else {
        return AVERROR(EINVAL);
    }

    s337_payload[5] = 0x00;
    uint16_t bitcount = pkt->size * 8;
    s337_payload[6] = bitcount & 0xff; /* Length code */
    s337_payload[7] = bitcount >> 8; /* Length code */
    s337_payload_start = &s337_payload[8];
    for (i = 0; i < pkt->size; i += 2) {
        s337_payload_start[0] = pkt->data[i+1];
        s337_payload_start[1] = pkt->data[i];
        s337_payload_start += 2;
    }

    return 0;
}

static int decklink_write_audio_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVStream *st = avctx->streams[pkt->stream_index];
    AVCodecParameters *c = st->codecpar;
    int sample_count;
    buffercount_type buffered;
    uint8_t *outbuf = NULL;
    int i, ret = 0;
    int interleave_offset = 0;
    int sample_offset, sample_size;
    uint64_t t1;
    struct AVPacketList *cur;
    int src_offset, remaining;

    t1 = av_vtune_get_timestamp();

    if (ctx->audio_st_lastpts[pkt->stream_index] != pkt->pts) {
        int64_t delta = pkt->pts - ctx->audio_st_lastpts[pkt->stream_index];
        if (delta > -AUDIO_PTS_FUDGEFACTOR && delta < AUDIO_PTS_FUDGEFACTOR) {
            /* Within the fudge factor, so just slip the packet's
               pts to match the where the last call left off */
            pkt->pts = ctx->audio_st_lastpts[pkt->stream_index];
        }
    }

    ctx->dlo->GetBufferedAudioSampleFrameCount(&buffered);
    if (ctx->playback_started && !buffered)
        av_log(avctx, AV_LOG_WARNING, "There's no buffered audio."
               " Audio will misbehave!\n");

    if (st->codecpar->codec_id == AV_CODEC_ID_AC3) {
        /* Encapsulate AC3 syncframe into SMPTE 337 packet */
        int outbuf_size;
        ret = create_s337_payload(pkt, st->codecpar->codec_id,
                                  &outbuf, &outbuf_size);
        if (ret != 0)
            goto done;
        sample_size = 4;
        sample_count = outbuf_size / 4;
    } else {
        sample_count = pkt->size / (c->channels << 1);
        sample_size = st->codecpar->channels * 2;
        outbuf = pkt->data;
    }

    /* Figure out the interleaving offset for this stream */
    for (i = 0; i < pkt->stream_index; i++) {
        AVStream *audio_st = avctx->streams[i];
        if (audio_st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            if (audio_st->codecpar->codec_id == AV_CODEC_ID_AC3)
                interleave_offset += 2;
            else
                interleave_offset += audio_st->codecpar->channels;
    }

    pthread_mutex_lock(&ctx->audio_mutex);
    if (ctx->audio_pkt_numsamples == 0) {
        /* Establish initial cadence */
        ctx->audio_pkt_numsamples = sample_count;
        ctx->output_audio_list = (AVPacketList *)av_mallocz(sizeof(AVPacketList));
        if (ctx->output_audio_list == NULL)
            goto done_unlock;
        ret = av_new_packet(&ctx->output_audio_list->pkt, ctx->audio_pkt_numsamples * ctx->channels * 2);
        if (ret != 0)
            goto done_unlock;
        memset(ctx->output_audio_list->pkt.data, 0, ctx->audio_pkt_numsamples * ctx->channels * 2);
        ctx->output_audio_list->pkt.pts = pkt->pts;
    }

    remaining = sample_count;
    src_offset = 0;
    for (cur = ctx->output_audio_list; cur != NULL; cur = cur->next) {
        /* See if we should interleave into this packet */
        if (((pkt->pts) >= cur->pkt.pts) &&
            (pkt->pts) < (cur->pkt.pts + ctx->audio_pkt_numsamples)) {
            int num_copy = remaining;
            int dst_offset = pkt->pts - cur->pkt.pts;

            /* Don't overflow dest buffer */
            if (num_copy > (ctx->audio_pkt_numsamples - dst_offset))
                num_copy = ctx->audio_pkt_numsamples - dst_offset;

            /* Yes, interleave */
            sample_offset = (dst_offset * ctx->channels + interleave_offset) * 2;
            for (i = 0; i < num_copy; i++) {
                memcpy(&cur->pkt.data[sample_offset], &outbuf[(i + src_offset) * sample_size], sample_size);
                sample_offset += (ctx->channels * 2);
            }
            pkt->pts += num_copy;
            src_offset += num_copy;
            remaining -= num_copy;
            if (remaining == 0)
                break;
        }

        if ((pkt->pts >= cur->pkt.pts) && cur->next == NULL && remaining > 0) {
            /* We need a new packet in our outgoing queue */
            cur->next = (AVPacketList *)av_mallocz(sizeof(AVPacketList));
            if (cur->next == NULL)
                goto done_unlock;
            ret = av_new_packet(&cur->next->pkt, ctx->audio_pkt_numsamples * ctx->channels * 2);
            if (ret != 0)
                goto done_unlock;
            memset(cur->next->pkt.data, 0, ctx->audio_pkt_numsamples * ctx->channels * 2);
            cur->next->pkt.pts = cur->pkt.pts + ctx->audio_pkt_numsamples;
        }
    }

    /* Stash the last PTS for the next call */
    ctx->audio_st_lastpts[pkt->stream_index] = pkt->pts;

done_unlock:
    pthread_mutex_unlock(&ctx->audio_mutex);
done:
    if (st->codecpar->codec_id == AV_CODEC_ID_AC3)
        av_free(outbuf);

    av_vtune_log_event("write_audio", t1, av_vtune_get_timestamp(), 1);

    return ret;
}

static int decklink_write_data_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;

    AVPacket *avpacket = av_packet_clone(pkt);
    if (avpacket_queue_put(&ctx->vanc_queue, avpacket) < 0) {
        av_log(avctx, AV_LOG_WARNING, "Failed to queue DATA packet\n");
    }

    return 0;
}

extern "C" {

av_cold int ff_decklink_write_header(AVFormatContext *avctx)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx;
    unsigned int n;
    uint64_t t1;
    int ret;
    char hostname[256];
    int port;
    int error;
    char sport[16];
    struct addrinfo hints = { 0 }, *res = 0;

    t1 = av_vtune_get_timestamp();

    ctx = (struct decklink_ctx *) av_mallocz(sizeof(struct decklink_ctx));
    if (!ctx)
        return AVERROR(ENOMEM);
    ctx->list_devices = cctx->list_devices;
    ctx->list_formats = cctx->list_formats;
    ctx->preroll      = cctx->preroll;
    cctx->ctx = ctx;
    ctx->udp_fd = -1;
#if CONFIG_LIBKLVANC
    klvanc_context_create(&ctx->vanc_ctx);
#endif

    /* List available devices and exit. */
    if (ctx->list_devices) {
        ff_decklink_list_devices_legacy(avctx, 0, 1);
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
            if (decklink_setup_data(avctx, st))
                goto error;
        } else {
            av_log(avctx, AV_LOG_ERROR, "Unsupported stream type.\n");
            goto error;
        }
    }

    /* Reconfigure the data stream clocks to match the video */
    for (n = 0; n < avctx->nb_streams; n++) {
        AVStream *st = avctx->streams[n];
        AVCodecParameters *c = st->codecpar;
        if (c->codec_type == AVMEDIA_TYPE_DATA)
            avpriv_set_pts_info(st, 64, ctx->bmd_tb_num, ctx->bmd_tb_den);
    }

    if (ctx->audio > 0) {
        if (decklink_enable_audio(avctx))
            goto error;
    }

    av_vtune_log_event("write_header", t1, av_vtune_get_timestamp(), 1);

    /* Setup the UDP monitor callback */

    if (cctx->udp_monitor) {
        av_url_split(NULL, 0, NULL, 0, hostname, sizeof(hostname), &port, NULL,
                     0, cctx->udp_monitor);

        /* This is all cribbed from libavformat/udp.c */
        snprintf(sport, sizeof(sport), "%d", port);
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_family   = AF_UNSPEC;

        if ((error = getaddrinfo(hostname, sport, &hints, &res))) {
            res = NULL;
            av_log(avctx, AV_LOG_ERROR, "getaddrinfo(%s, %s): %s\n",
                   hostname, sport, gai_strerror(error));
        } else {
            struct sockaddr_storage dest_addr;
            int dest_addr_len;

            memcpy(&dest_addr, res->ai_addr, res->ai_addrlen);
            dest_addr_len = res->ai_addrlen;

            ctx->udp_fd = ff_socket(res->ai_family, SOCK_DGRAM, 0);
            if (ctx->udp_fd < 0) {
                av_log(avctx, AV_LOG_ERROR, "Call to ff_socket failed\n");
            } else {
                if (connect(ctx->udp_fd, (struct sockaddr *) &dest_addr,
                            dest_addr_len)) {
                    av_log(avctx, AV_LOG_ERROR, "Failure to connect to monitor port\n");
                    closesocket(ctx->udp_fd);
                    ctx->udp_fd = -1;
                    goto error;
                }
            }
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

    if      (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        return decklink_write_video_packet(avctx, pkt);
    else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        return decklink_write_audio_packet(avctx, pkt);
    else if (st->codecpar->codec_type == AVMEDIA_TYPE_DATA) {
        return decklink_write_data_packet(avctx, pkt);
    }

    return AVERROR(EIO);
}

int ff_decklink_list_output_devices(AVFormatContext *avctx, struct AVDeviceInfoList *device_list)
{
    return ff_decklink_list_devices(avctx, device_list, 0, 1);
}

} /* extern "C" */
