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
#include "libavformat/mux.h"
#include "libavformat/ltnlog.h"
#include "libavcodec/bytestream.h"
#include "libavutil/frame.h"
#include "libavutil/internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/sei-timestamp.h"
#include "avdevice.h"
#include "thumbnail.h"
#if CONFIG_LIBZVBI
#include <libzvbi.h>
#endif
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

/*
  Debug logging levels
   1 = Low frequency events and correctness checks that should always pass
   2 = FIFO levels reported about once per second
   3 = FIFO levels reported on every audio/video packet received
   4 = General program flow (entry/exit of key functions)
*/

/* DeckLink callback class declaration */
class decklink_frame : public IDeckLinkVideoFrame, public IDeckLinkVideoFrameMetadataExtensions
{
public:
    decklink_frame(struct decklink_ctx *ctx, AVFrame *avframe, AVCodecID codec_id, int height, int width) :
        _ctx(ctx), _avframe(avframe), _avpacket(NULL), _codec_id(codec_id), _ancillary(NULL), _height(height), _width(width),  _refs(1) { }
    decklink_frame(struct decklink_ctx *ctx, AVPacket *avpacket, AVCodecID codec_id, int height, int width) :
        _ctx(ctx), _avframe(NULL), _avpacket(avpacket), _codec_id(codec_id), _ancillary(NULL), _height(height), _width(width), _colorspace(AVCOL_SPC_BT709), _eotf(AVCOL_TRC_BT709), hdr(NULL), lighting(NULL), _refs(1) { }
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
        if (_codec_id == AV_CODEC_ID_WRAPPED_AVFRAME) {
            return _avframe->linesize[0] < 0 ? bmdFrameFlagFlipVertical : bmdFrameFlagDefault;
        } else {
            if (_ctx->supports_hdr && (hdr || lighting))
                return bmdFrameFlagDefault | bmdFrameContainsHDRMetadata;
            else
                return bmdFrameFlagDefault;
        }
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
        if (_ancillary) {
            _ancillary->AddRef();
            return S_OK;
        } else {
            return S_FALSE;
        }
    }
    virtual HRESULT STDMETHODCALLTYPE SetAncillaryData(IDeckLinkVideoFrameAncillary *ancillary)
    {
        if (_ancillary)
            _ancillary->Release();
        _ancillary = ancillary;
        _ancillary->AddRef();
        return S_OK;
    }

    virtual HRESULT STDMETHODCALLTYPE SetMetadata(enum AVColorSpace colorspace, enum AVColorTransferCharacteristic eotf)
    {
        _colorspace = colorspace;
        _eotf = eotf;
        return S_OK;
    }

    // IDeckLinkVideoFrameMetadataExtensions interface
    virtual HRESULT GetInt(BMDDeckLinkFrameMetadataID metadataID, int64_t* value)
    {
        HRESULT result = S_OK;

        switch (metadataID) {
        case bmdDeckLinkFrameMetadataHDRElectroOpticalTransferFunc:
            /* See CTA-861-G Sec 6.9 Dynamic Range and Mastering */

            switch(_eotf) {
            case AVCOL_TRC_SMPTEST2084:
                /* PQ */
                *value = 2;
               break;
            case AVCOL_TRC_ARIB_STD_B67:
                /* Also known as "HLG" */
                *value = 3;
                break;
            case AVCOL_TRC_SMPTE170M:
            case AVCOL_TRC_SMPTE240M:
            case AVCOL_TRC_BT709:
            default:
                /* SDR */
                *value = 0;
               break;
            }
            break;

        case bmdDeckLinkFrameMetadataColorspace:
            if (!_ctx->supports_colorspace) {
                result = E_NOTIMPL;
                break;
            }
            switch(_colorspace) {
            case AVCOL_SPC_BT470BG:
            case AVCOL_SPC_SMPTE170M:
            case AVCOL_SPC_SMPTE240M:
                *value = bmdColorspaceRec601;
                break;
            case AVCOL_SPC_BT2020_CL:
            case AVCOL_SPC_BT2020_NCL:
                *value = bmdColorspaceRec2020;
                break;
            case AVCOL_SPC_BT709:
                *value = bmdColorspaceRec709;
                break;
            default:
                /* CTA 861-G Sec 5.1 says if unspecified, SD should default to
                   170M and both HD and 2160p should default to BT.709 */
                if (_ctx->bmd_height < 720)
                    *value = bmdColorspaceRec601;
                else
                    *value = bmdColorspaceRec709;
                break;
            }
            break;
        default:
            result = E_INVALIDARG;
        }

        return result;
    }
    virtual HRESULT GetFloat(BMDDeckLinkFrameMetadataID metadataID, double* value)
    {
        *value = 0;

        switch (metadataID) {
        case bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedX:
            if (hdr && hdr->has_primaries)
                *value = av_q2d(hdr->display_primaries[0][0]);
            break;
        case bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedY:
            if (hdr && hdr->has_primaries)
                *value = av_q2d(hdr->display_primaries[0][1]);
            break;
        case bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenX:
            if (hdr && hdr->has_primaries)
                *value = av_q2d(hdr->display_primaries[1][0]);
            break;
        case bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenY:
            if (hdr && hdr->has_primaries)
                *value = av_q2d(hdr->display_primaries[1][1]);
            break;
        case bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueX:
            if (hdr && hdr->has_primaries)
                *value = av_q2d(hdr->display_primaries[2][0]);
            break;
        case bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueY:
            if (hdr && hdr->has_primaries)
                *value = av_q2d(hdr->display_primaries[2][1]);
            break;
        case bmdDeckLinkFrameMetadataHDRWhitePointX:
            if (hdr && hdr->has_primaries)
                *value = av_q2d(hdr->white_point[0]);
            break;
        case bmdDeckLinkFrameMetadataHDRWhitePointY:
            if (hdr && hdr->has_primaries)
                *value = av_q2d(hdr->white_point[1]);
            break;
        case bmdDeckLinkFrameMetadataHDRMaxDisplayMasteringLuminance:
            if (hdr && hdr->has_luminance)
                *value = av_q2d(hdr->max_luminance);
            break;
        case bmdDeckLinkFrameMetadataHDRMinDisplayMasteringLuminance:
            if (hdr && hdr->has_luminance)
                *value = av_q2d(hdr->min_luminance);
            break;
        case bmdDeckLinkFrameMetadataHDRMaximumContentLightLevel:
            if (lighting)
                *value = (float) lighting->MaxCLL;
            else
                *value = 0;
            break;
        case bmdDeckLinkFrameMetadataHDRMaximumFrameAverageLightLevel:
            if (lighting)
                *value = (float) lighting->MaxFALL;
            else
                *value = 0;
            break;
        default:
            return E_INVALIDARG;
        }

        return S_OK;
    }

    virtual HRESULT GetFlag(BMDDeckLinkFrameMetadataID metadataID, bool* value)
    {
        *value = false;
        return E_INVALIDARG;
    }
    virtual HRESULT GetString(BMDDeckLinkFrameMetadataID metadataID, const char** value)
    {
        *value = nullptr;
        return E_INVALIDARG;
    }
    virtual HRESULT GetBytes(BMDDeckLinkFrameMetadataID metadataID, void* buffer, uint32_t* bufferSize)
    {
        *bufferSize = 0;
        return E_INVALIDARG;
    }

    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv)
    {
        CFUUIDBytes             iunknown;
        HRESULT                 result          = S_OK;

        if (!ppv)
            return E_INVALIDARG;

        *ppv = NULL;

        iunknown = CFUUIDGetUUIDBytes(IUnknownUUID);
        if (memcmp(&iid, &iunknown, sizeof(REFIID)) == 0) {
            *ppv = this;
            AddRef();
        } else if (memcmp(&iid, &IID_IDeckLinkVideoFrame, sizeof(REFIID)) == 0) {
            *ppv = static_cast<IDeckLinkVideoFrame*>(this);
            AddRef();
        } else if (memcmp(&iid, &IID_IDeckLinkVideoFrameMetadataExtensions, sizeof(REFIID)) == 0) {
            *ppv = static_cast<IDeckLinkVideoFrameMetadataExtensions*>(this);
            AddRef();
        } else {
            result = E_NOINTERFACE;
        }

        return result;
    }

    virtual ULONG   STDMETHODCALLTYPE AddRef(void)                            { return ++_refs; }
    virtual ULONG   STDMETHODCALLTYPE Release(void)
    {
        int ret = --_refs;
        if (!ret) {
            av_frame_free(&_avframe);
            av_packet_free(&_avpacket);
            if (_ancillary)
                _ancillary->Release();
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
    enum AVColorSpace _colorspace;
    enum AVColorTransferCharacteristic _eotf;
    const AVMasteringDisplayMetadata *hdr;
    const AVContentLightMetadata *lighting;

private:
    std::atomic<int>  _refs;
};

static void decklink_insert_frame(AVFormatContext *_avctx, struct decklink_cctx *_cctx,
                                  decklink_frame *frame, int64_t pts, int num_frames)
{
    struct decklink_ctx *ctx = (struct decklink_ctx *)_cctx->ctx;
    uint32_t buffered;
    uint32_t vid_buffered;
    BMDTimeValue streamtime;
    BMDTimeValue vid_streamtime;

    ctx->dlo->GetBufferedAudioSampleFrameCount(&buffered);
    ctx->dlo->GetBufferedVideoFrameCount(&vid_buffered);
    int ret = ctx->dlo->GetScheduledStreamTime(ctx->bmd_tb_den, &vid_streamtime, NULL);
    if (ret != 0) {
        av_log(_avctx, AV_LOG_WARNING, "Failed getting streamtime %d\n", ret);
    }

    av_log(_avctx, AV_LOG_WARNING, "Inserting %d frames (%d) (vid=%d)."
           " vid_streamtime=%ld.  Advancing %d audio samples\n",
           num_frames, buffered, vid_buffered,
           vid_streamtime / ctx->bmd_tb_num,
           ctx->audio_samples_per_frame * num_frames);

    ctx->dlo->GetScheduledStreamTime(48000, &streamtime, NULL);
    for (int i = 0; i < num_frames; i++) {
        uint32_t written;
        HRESULT result;

        pthread_mutex_lock(&ctx->mutex);
        while (ctx->frames_buffer_available_spots == 0) {
            pthread_cond_wait(&ctx->cond, &ctx->mutex);
        }
        ctx->frames_buffer_available_spots--;
        pthread_mutex_unlock(&ctx->mutex);

        ctx->video_offset++;
        ctx->frameCount++;
        result = ctx->dlo->ScheduleVideoFrame((class IDeckLinkVideoFrame *) frame,
                                              (pts + ctx->video_offset) * ctx->bmd_tb_num,
                                              ctx->bmd_tb_num, ctx->bmd_tb_den);
        if (result != S_OK) {
            av_log(_avctx, AV_LOG_ERROR, "Failed to schedule video frame: %d\n",
                   result);
        }
        result = ctx->dlo->ScheduleAudioSamples(ctx->empty_audio_buf,
                                                ctx->audio_samples_per_frame, streamtime + buffered,
                                                bmdAudioSampleRate48kHz,
                                                &written);
        if (result != S_OK) {
            av_log(_avctx, AV_LOG_ERROR, "Failed to schedule audio: %d written=%d\n",
                   result, written);
            ltnlog_stat("ERROR AUDIO", result);
        } else if (written != ctx->audio_samples_per_frame) {
            av_log(_avctx, AV_LOG_ERROR, "Audio write failure: requested=%d written=%d\n",
                   ctx->audio_samples_per_frame, written);
        } else {
            ltnlog_stat("PLAY AUDIO BYTES", written);
        }

        ctx->audio_offset += ctx->audio_samples_per_frame;
        buffered += ctx->audio_samples_per_frame;
    }
}

static void decklink_drop_frame(AVFormatContext *_avctx, struct decklink_cctx *_cctx,
                                int num_frames)
{
    struct decklink_ctx *ctx = (struct decklink_ctx *)_cctx->ctx;
    uint32_t buffered;
    uint32_t vid_buffered;

    ctx->dlo->GetBufferedAudioSampleFrameCount(&buffered);
    ctx->dlo->GetBufferedVideoFrameCount(&vid_buffered);
    av_log(_avctx, AV_LOG_WARNING, "Dropping %d frames (%d) (vid=%d).\n",
           num_frames, buffered, vid_buffered);

    ctx->video_offset -= num_frames;
    ctx->audio_offset -= ctx->audio_samples_per_frame * num_frames;
}

class decklink_output_callback : public IDeckLinkVideoOutputCallback, public IDeckLinkAudioOutputCallback
{
public:
    AVFormatContext *_avctx;
    int64_t last_audio_callback;

    decklink_output_callback(AVFormatContext *avctx) : _avctx(avctx), last_audio_callback(0) {}
    virtual HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame *_frame, BMDOutputFrameCompletionResult result)
    {
        decklink_frame *frame = static_cast<decklink_frame *>(_frame);
        struct decklink_cctx *cctx = (struct decklink_cctx *)_avctx->priv_data;
        struct decklink_ctx *ctx = frame->_ctx;

        if (frame->_avpacket) {
            uint8_t *side_data;
            size_t side_data_size;

            av_packet_update_pipelinestats(frame->_avpacket, AVFORMAT_OUTPUT_TIME,
                                           av_gettime(), -1, -1);
            side_data = av_packet_get_side_data(frame->_avpacket, AV_PKT_DATA_PIPELINE_STATS,
                                                &side_data_size);
            if (side_data) {
                struct AVPipelineStats *stats = (struct AVPipelineStats *) side_data;
                ltnlog_stat("VIDEOLATENCY_MS", (stats->avformat_output_time - stats->avformat_input_time) / 1000);
                if (cctx->latency_debug_level >= 1) {
                    av_log(_avctx, AV_LOG_INFO,
                           "in_pts=%" PRId64 " a=%" PRId64 " i=%" PRId64 " r=%" PRId64 " d=%" PRId64 " e=%" PRId64 " gs=%" PRId64 " ge=%" PRId64 " es=%" PRId64 " ee=%" PRId64 " wt=%" PRId64 " wm=%" PRId64 " o=%" PRId64 "\n",
                           stats->input_pts, stats->avprotocol_arrival_time,
                           stats->avformat_input_time, stats->avformat_read_time,
                           stats->avcodec_decode_start, stats->avcodec_decode_end,
                           stats->avfilter_graph_start, stats->avfilter_graph_end,
                           stats->avcodec_encode_start, stats->avcodec_encode_end,
                           stats->avformat_write_time, stats->avformat_mod_write_time,
                           stats->avformat_output_time);
                }
            }

            side_data = av_packet_get_side_data(frame->_avpacket, AV_PKT_DATA_SEI_UNREGISTERED,
                                                &side_data_size);
            if (side_data) {
                int offset = ltn_uuid_find(side_data, side_data_size);
                if (offset >= 0) {
                    struct timeval now, diff;
                    struct timeval encode_input, encode_output;
                    int64_t val;

                    memset(&encode_input, 0, sizeof(struct timeval));
                    memset(&encode_output, 0, sizeof(struct timeval));
                    sei_timestamp_value_timeval_query(side_data + offset, side_data_size - offset, 2, &encode_input);
                    sei_timestamp_value_timeval_query(side_data + offset, side_data_size - offset, 8, &encode_output);
                    gettimeofday(&now, NULL);

                    if (encode_output.tv_sec != 0) {
                        sei_timeval_subtract(&diff, &encode_output, &encode_input);
                        val = (diff.tv_sec * 1000) + (diff.tv_usec / 1000);
                    } else {
                        val = -1;
                    }
                    ltnlog_stat("ENCODETOTAL_MS", val);

                    sei_timeval_subtract(&diff, &now, &encode_input);
                    val = (diff.tv_sec * 1000) + (diff.tv_usec / 1000);
                    ltnlog_stat("GLASSTOGLASS_MS", val);
                }
            }
        }

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
            ltnlog_stat("VIDEOLATE", ctx->late);
            break;
        case bmdOutputFrameDropped:
            ctx->dropped++;
            av_log(_avctx, AV_LOG_WARNING, "Video buffer dropped\n");
            ltnlog_stat("VIDEODROP", ctx->dropped);
            break;
        }

        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped(void)       { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE RenderAudioSamples (bool preroll)
    {
        struct decklink_cctx *cctx = (struct decklink_cctx *)_avctx->priv_data;
        struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
        PacketListEntry *cur;
        BMDTimeValue streamtime;

        /* Make sure the callback is firing on schedule.  It may not be if the system is
           heavily loaded */
        if (cctx->debug_level >= 1) {
            int64_t current_run = av_gettime_relative();
            if (!preroll && last_audio_callback != 0 &&
                ((current_run - last_audio_callback > 25000) ||
                 (current_run - last_audio_callback < 18000)) ) {
                av_log(_avctx, AV_LOG_ERROR, "Audio callback not firing on schedule.  last=%ld current=%ld delta=%ld\n",
                       last_audio_callback, current_run, current_run - last_audio_callback);
            }
            last_audio_callback = current_run;
        }

        pthread_mutex_lock(&ctx->audio_mutex);

        ctx->dlo->GetScheduledStreamTime(48000, &streamtime, NULL);
        uint32_t buffered;
        ctx->dlo->GetBufferedAudioSampleFrameCount(&buffered);

        /* Do final Scheduling of audio at least 50ms before deadline.  This ensures there
           was enough time for multiple audio streams to be interleaved, while sending to
           the hardware with enough time for actual output. */
        int64_t window = streamtime + (bmdAudioSampleRate48kHz * 50 / 1000);

        if (preroll && ctx->audio_pkt_numsamples) {
            /* Throw away everything but the most recent 500ms.  This is to prevent
               failures that occur if you attempt to schedule more than 1 second of audio. */
            int total_pkts = 0;
            int keep_pkts = (bmdAudioSampleRate48kHz / 2) / ctx->audio_pkt_numsamples;
            int throwaway = 0;
            for (cur = ctx->output_audio_list.pkt_list.head; cur != NULL; cur = cur->next)
                total_pkts++;

            if (total_pkts > keep_pkts)
                throwaway = total_pkts - keep_pkts;

            while (throwaway > 0) {
                AVPacket pkt;
                ff_decklink_packet_queue_get(&ctx->output_audio_list, &pkt, 1);
                av_packet_unref(&pkt);
                throwaway--;
            }
        }

        while (1) {
            AVPacket pkt;
            int64_t cur_pts;

            cur_pts = ff_decklink_packet_queue_peekpts(&ctx->output_audio_list);

            if (cctx->debug_level >= 4 && preroll == 0)
                av_log(_avctx, AV_LOG_INFO, "Considering audio: pts=%ld ns=%d streamtime=%ld window=%ld next=%p delta=%ld buffered=%d\n",
                       cur_pts, ctx->audio_pkt_numsamples, streamtime, window, cur->next, window - cur_pts, buffered);

            if (cur_pts == -1 || (cur_pts > window) && !preroll)
                break;

            ff_decklink_packet_queue_get(&ctx->output_audio_list, &pkt, 1);

            if (cctx->debug_level >= 4)
                av_log(_avctx, AV_LOG_INFO, "Scheduling audio: pts=%ld ns=%d streamtime=%ld window=%ld\n",
                       pkt.pts, ctx->audio_pkt_numsamples, streamtime, window);

            uint32_t written;
            HRESULT result = ctx->dlo->ScheduleAudioSamples(pkt.data,
                                                            ctx->audio_pkt_numsamples, pkt.pts,
                                                            bmdAudioSampleRate48kHz,
                                                            &written);
            if (result != S_OK) {
                ltnlog_stat("ERROR AUDIO", result);
                av_log(_avctx, AV_LOG_ERROR, "Failed to schedule audio: %d written=%d\n",
                       result, written);
            } else if (written != ctx->audio_pkt_numsamples) {
                av_log(_avctx, AV_LOG_ERROR, "Audio write failure: pts=%ld requested=%d written=%d\n",
                       cur->pkt.pts, ctx->audio_pkt_numsamples, written);
            } else
                ltnlog_stat("PLAY AUDIO BYTES", written);

            av_packet_unref(&pkt);
        }

        if (!preroll) {
            uint32_t buffered;
            uint32_t vid_buffered;
            BMDTimeValue vid_streamtime;

            ctx->dlo->GetBufferedAudioSampleFrameCount(&buffered);
            ctx->dlo->GetBufferedVideoFrameCount(&vid_buffered);
            int ret = ctx->dlo->GetScheduledStreamTime(ctx->bmd_tb_den, &vid_streamtime, NULL);
            if (ret != 0) {
                av_log(_avctx, AV_LOG_WARNING, "Failed getting streamtime %d\n", ret);
            }

            ltnlog_stat("FIFO AUDIO BYTES", buffered);
            if (ctx->playback_started && buffered < (48000 / 50)){
                av_log(_avctx, AV_LOG_WARNING, "There's insufficient buffered audio (%d) (vid=%d)."
                       " Audio will misbehave! vid_streamtime=%ld\n", buffered, vid_buffered,
                       vid_streamtime / ctx->bmd_tb_num);
            }
        }

        pthread_mutex_unlock(&ctx->audio_mutex);

        return S_OK;
    }

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
    if (ctx->supports_vanc && ctx->dlo->EnableVideoOutput(ctx->bmd_mode, bmdVideoOutputVANC) != S_OK) {
        av_log(avctx, AV_LOG_WARNING, "Could not enable video output with VANC! Trying without...\n");
        ctx->supports_vanc = 0;
    }
    if (!ctx->supports_vanc && ctx->dlo->EnableVideoOutput(ctx->bmd_mode, bmdVideoOutputFlagDefault) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not enable video output!\n");
        return -1;
    }

    /* Set callback. */
    ctx->output_callback = new decklink_output_callback(avctx);
    ctx->dlo->SetScheduledFrameCompletionCallback(ctx->output_callback);
    ctx->dlo->SetAudioCallback(ctx->output_callback);
    ctx->audio_samples_per_frame = bmdAudioSampleRate48kHz * st->time_base.num / st->time_base.den;

    ctx->frames_preroll = ceil(st->time_base.den * ctx->preroll / st->time_base.num);
    if (ctx->frames_preroll < 3) {
        /* No matter what they specify as the preroll, we can't support lower than three
           frames due to the way the hardware queueing works */
        ctx->frames_preroll = 3;
    }

    /* Buffer twice as many frames as the preroll. */
    ctx->frames_preroll = FFMIN(ctx->frames_preroll, 30);
    ctx->frames_buffer = ctx->frames_preroll * 2;

    ltnlog_stat("PREROLL_TARGET", ctx->frames_preroll * st->time_base.num * 1000 / st->time_base.den);

    /* Throw the first X frames so that all upstream FIFOs have the opportunity
       to flush (to reduce realtime latency) */
    ctx->frames_discard = st->time_base.den * cctx->discard / st->time_base.num;

    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_mutex_init(&ctx->audio_mutex, NULL);
    pthread_cond_init(&ctx->cond, NULL);
    ctx->frames_buffer_available_spots = ctx->frames_buffer;

    av_log(avctx, AV_LOG_DEBUG, "output: %s, preroll: %d, frames buffer size: %d\n",
           avctx->url, ctx->frames_preroll, ctx->frames_buffer);

    /* The device expects the framerate to be fixed. */
    avpriv_set_pts_info(st, 64, st->time_base.num, st->time_base.den);

    if (cctx->thumbnail_filename) {
        thumbnail_init(&ctx->thumbnail_ctx, cctx->thumbnail_filename,
                       ctx->bmd_width, ctx->bmd_height, 320, 180,
                       cctx->thumbnail_quality);
        ctx->thumbnail_frames = ceil(st->time_base.den * cctx->thumbnail_interval
                                     / st->time_base.num);
    }

    ctx->video = 1;

    return 0;
}

static int decklink_setup_audio(AVFormatContext *avctx, AVStream *st)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVCodecParameters *c = st->codecpar;

    if (c->codec_id == AV_CODEC_ID_AC3) {
        /* Regardless of the number of channels in the codec, we're only
           using 2 SDI audio channels at 48000Hz */
        ctx->channels += 2;
    } else if (c->codec_id == AV_CODEC_ID_PCM_S16LE) {
        if (c->sample_rate != 48000) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported sample rate!"
                   " Only 48kHz is supported.\n");
            return -1;
        }
        ctx->channels += c->ch_layout.nb_channels;
    } else {
        av_log(avctx, AV_LOG_ERROR, "Unsupported codec specified!"
               " Only PCM_S16LE and AC-3 are supported.\n");
        return -1;
    }

    /* The device expects the sample rate to be fixed. */
    avpriv_set_pts_info(st, 64, 1, 48000);

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

    ctx->empty_audio_buf = malloc(ctx->audio_samples_per_frame * ctx->channels * 2);
    if (!ctx->empty_audio_buf)
        return -1;

    if (ctx->dlo->EnableAudioOutput(bmdAudioSampleRate48kHz,
                                    bmdAudioSampleType16bitInteger,
                                    ctx->channels,
                                    bmdAudioOutputStreamTimestamped) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not enable audio output!\n");
        return -1;
    }

    return 0;
}

/* Wrap the AC-3 packet into an S337 payload that is in S16LE format which can be easily
   injected into the PCM stream.  Note: despite the function name, only AC-3 is implemented */
static int create_s337_payload(AVPacket *pkt, uint8_t **outbuf, int *outsize)
{
    /* Note: if the packet size is not divisible by four, we need to make the actual
       payload larger to ensure it ends on an two channel S16LE boundary */
    int payload_size = FFALIGN(pkt->size, 4) + 8;
    uint16_t bitcount = pkt->size * 8;
    uint8_t *s337_payload;
    PutByteContext pb;

    /* Sanity check:  According to SMPTE ST 340:2015 Sec 4.1, the AC-3 sync frame will
       exactly match the 1536 samples of baseband (PCM) audio that it represents.  */
    if (pkt->size > 1536)
        return AVERROR(EINVAL);

    /* Encapsulate AC3 syncframe into SMPTE 337 packet */
    s337_payload = (uint8_t *) av_malloc(payload_size);
    if (s337_payload == NULL)
        return AVERROR(ENOMEM);
    bytestream2_init_writer(&pb, s337_payload, payload_size);
    bytestream2_put_le16u(&pb, 0xf872); /* Sync word 1 */
    bytestream2_put_le16u(&pb, 0x4e1f); /* Sync word 1 */
    bytestream2_put_le16u(&pb, 0x0001); /* Burst Info, including data type (1=ac3) */
    bytestream2_put_le16u(&pb, bitcount); /* Length code */
    for (int i = 0; i < (pkt->size - 1); i += 2)
        bytestream2_put_le16u(&pb, (pkt->data[i] << 8) | pkt->data[i+1]);

    /* Ensure final payload is aligned on 4-byte boundary */
    if (pkt->size & 1)
        bytestream2_put_le16u(&pb, pkt->data[pkt->size - 1] << 8);
    if ((pkt->size & 3) == 1 || (pkt->size & 3) == 2)
        bytestream2_put_le16u(&pb, 0);

    *outsize = payload_size;
    *outbuf = s337_payload;
    return 0;
}

static int decklink_setup_subtitle(AVFormatContext *avctx, AVStream *st)
{
    int ret = -1;

    switch(st->codecpar->codec_id) {
#if CONFIG_LIBKLVANC
    case AV_CODEC_ID_EIA_608:
        /* No special setup required */
        ret = 0;
        break;
#endif
    default:
        av_log(avctx, AV_LOG_ERROR, "Unsupported subtitle codec specified\n");
        break;
    }

    return ret;
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
        if (ff_stream_add_bitstream_filter(st, "scte35toscte104", NULL) > 0) {
            st->codecpar->codec_id = AV_CODEC_ID_SCTE_104;
            ret = 0;
        } else {
            av_log(avctx, AV_LOG_ERROR, "SCTE-35 requires scte35toscte104 BSF to be available\n");
        }
        break;
#endif
    default:
        av_log(avctx, AV_LOG_ERROR, "Unsupported data codec specified\n");
        break;
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

    if (cctx->thumbnail_filename)
        thumbnail_shutdown(&ctx->thumbnail_ctx);

    av_log(avctx, AV_LOG_INFO, "Final stats: late=%d dropped=%d vo=%d ao=%d\n",
           ctx->late, ctx->dropped, ctx->video_offset, ctx->audio_offset);

    ff_decklink_cleanup(avctx);

    if (ctx->output_callback)
        delete ctx->output_callback;

    pthread_mutex_destroy(&ctx->mutex);
    pthread_cond_destroy(&ctx->cond);
    av_freep(&ctx->audio_st_lastpts);

#if CONFIG_LIBKLVANC
    klvanc_context_destroy(ctx->vanc_ctx);
#endif
    ff_decklink_packet_queue_end(&ctx->vanc_queue);

    ff_ccfifo_uninit(&ctx->cc_fifo);
    av_freep(&cctx->ctx);

    return 0;
}

#if CONFIG_LIBKLVANC
static void construct_cc(AVFormatContext *avctx, struct decklink_ctx *ctx,
                         AVPacket *pkt, struct klvanc_line_set_s *vanc_lines)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct klvanc_packet_eia_708b_s *cdp;
    uint16_t *cdp_words;
    uint16_t len;
    uint8_t cc_count;
    size_t size;
    int ret, i;

    if (cctx->cea708_line == -1)
        return;

    const uint8_t *data = av_packet_get_side_data(pkt, AV_PKT_DATA_A53_CC, &size);
    if (!data)
        return;

    cc_count = size / 3;

    ret = klvanc_create_eia708_cdp(&cdp);
    if (ret)
        return;

    ret = klvanc_set_framerate_EIA_708B(cdp, ctx->bmd_tb_num, ctx->bmd_tb_den);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Invalid framerate specified: %" PRId64 "/%" PRId64 "\n",
               ctx->bmd_tb_num, ctx->bmd_tb_den);
        klvanc_destroy_eia708_cdp(cdp);
        return;
    }

    if (cc_count > KLVANC_MAX_CC_COUNT) {
        av_log(avctx, AV_LOG_ERROR, "Illegal cc_count received: %d\n", cc_count);
        cc_count = KLVANC_MAX_CC_COUNT;
    }

    /* CC data */
    cdp->header.ccdata_present = 1;
    cdp->header.caption_service_active = 1;
    cdp->ccdata.cc_count = cc_count;
    for (i = 0; i < cc_count; i++) {
        if (data [3*i] & 0x04)
            cdp->ccdata.cc[i].cc_valid = 1;
        cdp->ccdata.cc[i].cc_type = data[3*i] & 0x03;
        cdp->ccdata.cc[i].cc_data[0] = data[3*i+1];
        cdp->ccdata.cc[i].cc_data[1] = data[3*i+2];
    }

    klvanc_finalize_EIA_708B(cdp, ctx->cdp_sequence_num++);
    ret = klvanc_convert_EIA_708B_to_words(cdp, &cdp_words, &len);
    klvanc_destroy_eia708_cdp(cdp);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed converting 708 packet to words\n");
        return;
    }

    ret = klvanc_line_insert(ctx->vanc_ctx, vanc_lines, cdp_words, len, cctx->cea708_line, 0);
    free(cdp_words);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "VANC line insertion failed\n");
        return;
    }
    ltnlog_stat("CC COUNT", cc_count);
}

/* See SMPTE ST 2016-3:2009 */
static void construct_afd(AVFormatContext *avctx, struct decklink_ctx *ctx,
                          AVPacket *pkt, struct klvanc_line_set_s *vanc_lines,
                          AVStream *st)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct klvanc_packet_afd_s *afd = NULL;
    uint16_t *afd_words = NULL;
    uint16_t len;
    size_t size;
    int f1_line = cctx->afd_line, f2_line = 0, ret;

    if (cctx->afd_line == -1)
        return;

    const uint8_t *data = av_packet_get_side_data(pkt, AV_PKT_DATA_AFD, &size);
    if (!data || size == 0)
        return;

    ret = klvanc_create_AFD(&afd);
    if (ret)
        return;

    ret = klvanc_set_AFD_val(afd, data[0]);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Invalid AFD value specified: %d\n",
               data[0]);
        klvanc_destroy_AFD(afd);
        return;
    }

    /* Compute the AR flag based on the DAR (see ST 2016-1:2009 Sec 9.1).  Note, we treat
       anything below 1.4 as 4:3 (as opposed to the standard 1.33), because there are lots
       of streams in the field that aren't *exactly* 4:3 but a tiny bit larger after doing
       the math... */
    if (av_cmp_q((AVRational) {st->codecpar->width * st->codecpar->sample_aspect_ratio.num,
                    st->codecpar->height * st->codecpar->sample_aspect_ratio.den}, (AVRational) {14, 10}) == 1)
        afd->aspectRatio = ASPECT_16x9;
    else
        afd->aspectRatio = ASPECT_4x3;

    ret = klvanc_convert_AFD_to_words(afd, &afd_words, &len);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Failed converting AFD packet to words\n");
        goto out;
    }

    ret = klvanc_line_insert(ctx->vanc_ctx, vanc_lines, afd_words, len, f1_line, 0);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "VANC line insertion failed\n");
        goto out;
    }

    /* For interlaced video, insert into both fields.  Switching lines for field 2
       derived from SMPTE RP 168:2009, Sec 6, Table 2. */
    switch (ctx->bmd_mode) {
    case bmdModeNTSC:
    case bmdModeNTSC2398:
        f2_line = 273 - 10 + f1_line;
        break;
    case bmdModePAL:
        f2_line = 319 - 6 + f1_line;
        break;
    case bmdModeHD1080i50:
    case bmdModeHD1080i5994:
    case bmdModeHD1080i6000:
        f2_line = 569 - 7 + f1_line;
        break;
    default:
        f2_line = 0;
        break;
    }

    if (f2_line > 0) {
        ret = klvanc_line_insert(ctx->vanc_ctx, vanc_lines, afd_words, len, f2_line, 0);
        if (ret) {
            av_log(avctx, AV_LOG_ERROR, "VANC line insertion failed\n");
            goto out;
        }
    }

    ltnlog_stat("AFD", data[0]);

out:
    if (afd)
        klvanc_destroy_AFD(afd);
    if (afd_words)
        free(afd_words);
}

/* Parse any EIA-608 subtitles sitting on the queue, and write packet side data
   that will later be handled by construct_cc... */
static void parse_608subs(AVFormatContext *avctx, struct decklink_ctx *ctx, AVPacket *pkt)
{
    size_t cc_size = ff_ccfifo_getoutputsize(&ctx->cc_fifo);
    uint8_t *cc_data;

    if (!ff_ccfifo_ccdetected(&ctx->cc_fifo))
        return;

    cc_data = av_packet_new_side_data(pkt, AV_PKT_DATA_A53_CC, cc_size);
    if (cc_data)
        ff_ccfifo_injectbytes(&ctx->cc_fifo, cc_data, cc_size);
}

static int decklink_construct_vanc(AVFormatContext *avctx, struct decklink_ctx *ctx,
                                   AVPacket *pkt, decklink_frame *frame,
                                   AVStream *st)
{
    struct klvanc_line_set_s vanc_lines = { 0 };
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    int ret = 0, i;

    if (!ctx->supports_vanc)
        return 0;

    parse_608subs(avctx, ctx, pkt);
    construct_cc(avctx, ctx, pkt, &vanc_lines);
    construct_afd(avctx, ctx, pkt, &vanc_lines, st);

    /* See if there any pending data packets to process */
    while (ff_decklink_packet_queue_size(&ctx->vanc_queue) > 0) {
        AVStream *vanc_st;
        AVPacket vanc_pkt;
        int64_t pts;

        pts = ff_decklink_packet_queue_peekpts(&ctx->vanc_queue);
        if (pts > ctx->last_pts) {
            /* We haven't gotten to the video frame we are supposed to inject
               the oldest VANC packet into yet, so leave it on the queue... */
            break;
        }

        ret = ff_decklink_packet_queue_get(&ctx->vanc_queue, &vanc_pkt, 1);
        if (vanc_pkt.pts + 1 < ctx->last_pts) {
            av_log(avctx, AV_LOG_WARNING, "VANC packet too old, throwing away\n");
            av_packet_unref(&vanc_pkt);
            continue;
        }

        vanc_st = avctx->streams[vanc_pkt.stream_index];
        if (vanc_st->codecpar->codec_id == AV_CODEC_ID_SMPTE_2038) {
            struct klvanc_smpte2038_anc_data_packet_s *pkt_2038 = NULL;

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
	    /* SCTE-104 packets cannot be directly embedded into SDI.  They needs to
               be encapsulated in SMPTE 2010 first) */
            uint8_t *smpte2010_bytes;
            uint16_t smpte2010_len;

            if (cctx->scte104_line == -1) {
                av_packet_unref(&vanc_pkt);
                continue;
            }

            /* There is a known limitation in the libklvanc ST2010 generator where it
               cannot create payloads that span multiple packets.  For now just discard
               those messages */
            if (vanc_pkt.size > 254) {
                av_log(avctx, AV_LOG_INFO,
                       "SCTE-104 message exceeds ST2010 maximum and cannot be output.  Size=%d\n",
                       vanc_pkt.size);
                av_packet_unref(&vanc_pkt);
                continue;
            }

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
        ret = AVERROR(EIO);
        goto done;
    }

    /* Now that we've got all the VANC lines in a nice orderly manner, generate the
       final VANC sections for the Decklink output */
    for (i = 0; i < vanc_lines.num_lines; i++) {
        struct klvanc_line_s *line = vanc_lines.lines[i];
        int real_line;
        void *buf;

        if (!line)
            break;

        /* FIXME: include hack for certain Decklink cards which mis-represent
           line numbers for pSF frames */
        real_line = line->line_number;

        result = vanc->GetBufferForVerticalBlankingLine(real_line, &buf);
        if (result != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get VANC line %d: %d", real_line, result);
            continue;
        }

        /* Generate the full line taking into account all VANC packets on that line */
        result = klvanc_generate_vanc_line_v210(ctx->vanc_ctx, line, (uint8_t *) buf,
                                                ctx->bmd_width);
        if (result) {
            av_log(avctx, AV_LOG_ERROR, "Failed to generate VANC line\n");
            continue;
        }
    }

#if CONFIG_LIBZVBI
    /* ZVBI encoding of CC waveform */
    if (ctx->bmd_mode == bmdModeNTSC && cctx->cea608_vbi == 1) {
        const uint8_t *data;
        int ret;
        size_t size;
        void *out_line;

        data = av_packet_get_side_data(pkt, AV_PKT_DATA_A53_CC, &size);
        if (data) {
            uint8_t cc_count = size / 3;
            uint8_t ccf1[2] = { 0x80, 0x80 };
            uint8_t ccf2[2] = { 0x80, 0x80 };
            vbi_sampling_par sp;
            unsigned int raw_size;
            unsigned int blank_level;
            unsigned int black_level;
            unsigned int white_level;
            vbi_bool success;

            for (size_t i = 0; i < cc_count; i++) {
                uint8_t cc_type = data[3*i] & 0x03;

                if (cc_type == 0x00) {
                    ccf1[0] = data[3*i+1];
                    ccf1[1] = data[3*i+2];
                } else if (cc_type == 0x01) {
                    ccf2[0] = data[3*i+1];
                    ccf2[1] = data[3*i+2];
                }
            }

            /* ZVBI parameters */
            sp.scanning = 525;
            sp.sampling_format = VBI_PIXFMT_YUV420;
            sp.sampling_rate = 27000000;
            sp.bytes_per_line = 1440;
            sp.offset = (int)(9.7e-6 * sp.sampling_rate);
            sp.start[0] = 21;
            sp.count[0] = 1;
            sp.start[1] = 284;
            sp.count[1] = 1;
            sp.interlaced = TRUE;
            sp.synchronous = TRUE;
            blank_level = 16;
            black_level = 20;
            white_level = 235;

            raw_size = (sp.count[0] + sp.count[1]) * sp.bytes_per_line;
            uint8_t *raw = (uint8_t *) malloc (raw_size);
            if (raw == NULL)
                return -1;

            vbi_sliced sliced[2];
            sliced[0].id = VBI_SLICED_CAPTION_525_F1;
            sliced[0].line = 21;
            sliced[0].data[0] = ccf1[0];
            sliced[0].data[1] = ccf1[1];
            sliced[1].id = VBI_SLICED_CAPTION_525_F2;
            sliced[1].line = 284;
            sliced[1].data[0] = ccf2[0];
            sliced[1].data[1] = ccf2[1];

            success = vbi_raw_video_image (raw, raw_size, &sp,
                                           blank_level,
                                           black_level,
                                           white_level,
                                           0xff, FALSE,
                                           sliced,
                                           sizeof(sliced) / sizeof(vbi_sliced));
            if (success == TRUE) {
                uint16_t vbi_21_284[2880];
                for (int i = 0; i < 2880; i += 2) {
                    vbi_21_284[i] = 0x80;
                    vbi_21_284[i + 1] = raw[i];
                }

                /* Scale up 8-bit samples for both lines to 10-bit */
                for (int i = 0; i < 2880; i++)
                    vbi_21_284[i] = vbi_21_284[i] << 2;

                result = vanc->GetBufferForVerticalBlankingLine(21, &out_line);
                if (result == S_OK)
                    klvanc_uyvy_to_v210(vbi_21_284, (uint8_t *) out_line, 1440);
                result = vanc->GetBufferForVerticalBlankingLine(284, &out_line);
                if (result == S_OK)
                    klvanc_uyvy_to_v210(vbi_21_284+1440, (uint8_t *) out_line, 1440);
            }
            free(raw);
        }
    }
#endif

    result = frame->SetAncillaryData(vanc);
    vanc->Release();
    if (result != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set vanc: %d", result);
        ret = AVERROR(EIO);
    }

done:
    for (i = 0; i < vanc_lines.num_lines; i++)
        klvanc_line_free(vanc_lines.lines[i]);

    return ret;
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
    uint32_t buffered;
    HRESULT hr;

    ctx->last_pts = FFMAX(ctx->last_pts, pkt->pts);

    BMDTimeValue streamtime;
    int64_t delta;
    ctx->dlo->GetScheduledStreamTime(ctx->bmd_tb_den, &streamtime, NULL);
    delta = pkt->pts + ctx->video_offset - (streamtime / ctx->bmd_tb_num);

    if (ctx->frames_discard-- > 0) {
        av_log(avctx, AV_LOG_DEBUG, "Discarding frame with PTS %" PRId64 " discard=%d\n",
               pkt->pts, ctx->frames_discard);
        av_frame_free(&avframe);
        av_packet_free(&avpacket);
        return 0;
    }

    av_packet_update_pipelinestats(pkt, AVFORMAT_MOD_WRITE_TIME, av_gettime(), -1, -1);

    if (ctx->playback_started && (delta < 0 || delta > ctx->frames_buffer)) {
        /* We're behind realtime, or way too far ahead, so restart clocks */
        ltnlog_stat("OUTPUT RESTART", ++ctx->output_restart);
        av_log(avctx, AV_LOG_ERROR, "Scheduled frames received too %s.  "
               "Restarting output.  Delta=%" PRId64 "\n", delta < 0 ? "late" : "far into future", delta);
        if (ctx->dlo->StopScheduledPlayback(0, NULL, 0) != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to stop scheduled playback\n");
            return AVERROR(EIO);
        }
        if (ctx->audio) {
            ctx->dlo->DisableAudioOutput();
            free(ctx->empty_audio_buf);
        }

        ctx->frames_discard = st->time_base.den * cctx->discard / st->time_base.num;
        ctx->first_pts = AV_NOPTS_VALUE;
        ctx->playback_started = 0;
        ctx->audio_offset = 0;
        ctx->video_offset = 0;
        ctx->framebuffer_level = 0;
        ctx->num_framebuffer_level = 0;
        if (ctx->audio)
            if (decklink_enable_audio(avctx)) {
                av_log(avctx, AV_LOG_ERROR, "Error enabling audio\n");
            }

        /* Bail out after discarding the frame */
        av_frame_free(&avframe);
        av_packet_free(&avpacket);
        return 0;
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
        if (decklink_construct_vanc(avctx, ctx, pkt, frame, st))
            av_log(avctx, AV_LOG_ERROR, "Failed to construct VANC\n");
#endif
    }

    if (!frame) {
        av_log(avctx, AV_LOG_ERROR, "Could not create new frame.\n");
        av_frame_free(&avframe);
        av_packet_free(&avpacket);
        return AVERROR(EIO);
    }

    /* Set frame metadata properties */
    size_t size;
    const AVMasteringDisplayMetadata *hdr = (const AVMasteringDisplayMetadata *) av_packet_get_side_data(pkt, AV_PKT_DATA_MASTERING_DISPLAY_METADATA, &size);
    if (hdr && size > 0)
        frame->hdr = hdr;

    const AVContentLightMetadata *lighting = (const AVContentLightMetadata *) av_packet_get_side_data(pkt, AV_PKT_DATA_CONTENT_LIGHT_LEVEL, &size);
    if (hdr && size > 0)
        frame->lighting = lighting;

    frame->SetMetadata(st->codecpar->color_space, st->codecpar->color_trc);

    /* Always keep at most one second of frames buffered. */
    pthread_mutex_lock(&ctx->mutex);
    while (ctx->frames_buffer_available_spots == 0) {
        pthread_cond_wait(&ctx->cond, &ctx->mutex);
    }
    ctx->frames_buffer_available_spots--;
    pthread_mutex_unlock(&ctx->mutex);

    if (ctx->first_pts == AV_NOPTS_VALUE)
        ctx->first_pts = pkt->pts;

    if (cctx->thumbnail_filename && (ctx->frameCount % ctx->thumbnail_frames == 0))
        thumbnail_generate(&ctx->thumbnail_ctx, pkt);

    /* Schedule frame for playback. */
    ctx->frameCount++;
    hr = ctx->dlo->ScheduleVideoFrame((class IDeckLinkVideoFrame *) frame,
                                      (pkt->pts + ctx->video_offset) * ctx->bmd_tb_num,
                                      ctx->bmd_tb_num, ctx->bmd_tb_den);

    ctx->dlo->GetBufferedVideoFrameCount(&buffered);
    if (cctx->debug_level >= 3)
        av_log(avctx, AV_LOG_INFO, "Buffered video frames: %d (offset=%d) pts=%ld streamtime=%ld latency=%ld\n",
               (int) buffered, ctx->video_offset, pkt->pts, (streamtime / ctx->bmd_tb_num),
               (pkt->pts + ctx->video_offset) - (streamtime / ctx->bmd_tb_num));

    if (pkt->pts > (ctx->first_pts + 2) && buffered <= 2)
        av_log(avctx, AV_LOG_WARNING, "There are not enough buffered video frames."
               " Video may misbehave!\n");


    /* Make sure there is at least 60ms worth of data */
    int num_frames = (60 * ctx->bmd_tb_den / ctx->bmd_tb_num / 1000) + 1;
    if (pkt->pts > (ctx->first_pts + num_frames) && buffered <= num_frames) {
        av_log(avctx, AV_LOG_WARNING, "There are not enough buffered video frames to support audio."
               " Video/audio may misbehave!\n");
    }

    /* Adjust number of buffers queued to closely track preroll depth (to achieve
       desired target latency).  Because we only move up to one frame per minute, this is
       intended to compesnate for SDI clocks which are slightly too slow or too fast
       (i.e. milliseconds per hour of drift).  */
    time_t cur_time;
    time(&cur_time);
    if (cctx->decklink_live && ctx->last_framebuffer_level != cur_time) {
        ctx->framebuffer_level += buffered;
        ctx->num_framebuffer_level++;
        if (ctx->num_framebuffer_level > 59) {
            /* It's been a minute, compute the average */
            float fb_level = (float) ctx->framebuffer_level / (float) ctx->num_framebuffer_level;
            if (cctx->debug_level >= 1)
                av_log(avctx, AV_LOG_INFO, "Latency slipper: %d/%d=%f\n", ctx->framebuffer_level,
                       ctx->num_framebuffer_level, fb_level);
            if (fb_level > ctx->frames_preroll + 1) {
                /* Drop a frame to bring us closer to expected latency level */
                ltnlog_stat("OUTPUT SLIP", ++ctx->output_slipped);
                decklink_drop_frame(avctx, cctx, 1);
            } else if (fb_level < ctx->frames_preroll - 1) {
                ltnlog_stat("OUTPUT SLIP", ++ctx->output_slipped);
                decklink_insert_frame(avctx, cctx, frame, pkt->pts, 1);
            }

            ctx->framebuffer_level = 0;
            ctx->num_framebuffer_level = 0;
        }
        ctx->last_framebuffer_level = cur_time;
    }

    /* Pass ownership to DeckLink, or release on failure */
    frame->Release();
    if (hr != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not schedule video frame."
                " error %08x.\n", (uint32_t) hr);
        return AVERROR(EIO);
    }

    ltnlog_stat("PICTURE", pkt->pts);

    ctx->dlo->GetBufferedVideoFrameCount(&buffered);
    av_log(avctx, AV_LOG_DEBUG, "Buffered video frames: %d.\n", (int) buffered);
    if (pkt->pts > 2 && buffered <= 2)
        av_log(avctx, AV_LOG_WARNING, "There are not enough buffered video frames."
               " Video may misbehave!\n");

    /* Preroll video frames. */
    if (!ctx->playback_started) {
        if (pkt->pts >= (ctx->first_pts + ctx->frames_preroll - 3)) {
            /* We're about to start playback so start audio preroll */
            av_log(avctx, AV_LOG_DEBUG, "Starting audio preroll...\n");
            if (ctx->audio && ctx->dlo->BeginAudioPreroll() != S_OK) {
                av_log(avctx, AV_LOG_ERROR, "Could not begin audio preroll!\n");
                return -1;
            }
        }
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
    }

    /* Once per second, update the reported status of the Reference Input */
    time(&cur_time);
    if (ctx->last_refstatus_report != cur_time) {
        int64_t ref_mode = 0;
        ctx->status->GetInt(bmdDeckLinkStatusReferenceSignalMode, &ref_mode);
        ltnlog_stat("REFERENCESIGNALMODE", ref_mode);
        ctx->last_refstatus_report = cur_time;
    }

    return 0;
}

static int decklink_write_audio_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVStream *st = avctx->streams[pkt->stream_index];
    AVCodecParameters *c = st->codecpar;
    AVPacket pkt_new, *cur_pkt;
    int sample_count;
    uint32_t buffered;
    uint8_t *outbuf = NULL;
    int interleave_offset = 0;
    int sample_offset, sample_size;
    struct PacketListEntry *cur;
    int src_offset, remaining;
    int64_t cur_pts;
    int ret = 0;

    if (ctx->audio_st_lastpts[pkt->stream_index] != pkt->pts) {
        int64_t delta = pkt->pts - ctx->audio_st_lastpts[pkt->stream_index];

        if (cctx->debug_level >= 1 && ctx->audio_st_lastpts[pkt->stream_index] != 0) {
            av_log(avctx, AV_LOG_INFO, "Audio packet discontinuity expected=%ld received=%ld\n",
                   ctx->audio_st_lastpts[pkt->stream_index], pkt->pts);
        }
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
        ret = create_s337_payload(pkt, &outbuf, &outbuf_size);
        if (ret < 0)
            return ret;
        sample_size = 4;
        sample_count = outbuf_size / sample_size;
    } else {
        sample_size = c->ch_layout.nb_channels * 2;
        sample_count = pkt->size / sample_size;
        outbuf = pkt->data;
    }

    /* Figure out the interleaving offset for this stream */
    for (int i = 0; i < pkt->stream_index; i++) {
        AVStream *audio_st = avctx->streams[i];
        if (audio_st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            if (audio_st->codecpar->codec_id == AV_CODEC_ID_AC3)
                interleave_offset += 2;
            else
                interleave_offset += audio_st->codecpar->ch_layout.nb_channels;
    }

    /* Compute the dBFS for the audio channels in this stream */
    for (int i = 0; i < st->codecpar->ch_layout.nb_channels; i++) {
        int16_t largest_sample = 0;
        float dbfs, val;
        /* Find largest sample */
        int sample_offset = 0;
        for (int j = 0; j < sample_count; j++) {
            int offset = sample_offset + (i * 2);
            int16_t samp = outbuf[offset] | (outbuf[offset + 1] << 8);
            if (largest_sample < samp) {
                largest_sample = samp;
            }
            sample_offset += sample_size;
        }
        if (largest_sample == 0) {
            dbfs = -60;
        } else {
            val = largest_sample;
            dbfs = 20 * log10(val / 32767.0);
        }
        ltnlog_msg("AUDIO DBFS", "%d,%f\n", interleave_offset + i, dbfs);
    }

    pthread_mutex_lock(&ctx->audio_mutex);
    if (ctx->audio_pkt_numsamples == 0) {
        /* Establish initial cadence */
        ff_decklink_packet_queue_init(avctx, &ctx->output_audio_list, cctx->audio_queue_size);
        if (cctx->debug_level >= 1)
            av_log(avctx, AV_LOG_INFO, "Initial cadence audio sample count=%d\n", sample_count);
        ctx->audio_pkt_numsamples = sample_count;
    }

    if (ff_decklink_packet_queue_size(&ctx->output_audio_list) == 0) {
        ret = av_new_packet(&pkt_new, ctx->audio_pkt_numsamples * ctx->channels * 2);
        if (ret != 0)
            goto done_unlock;
        memset(pkt_new.data, 0, ctx->audio_pkt_numsamples * ctx->channels * 2);
        pkt_new.pts = pkt->pts;
        ff_decklink_packet_queue_put(&ctx->output_audio_list, &pkt_new);
    }

    cur_pts = ff_decklink_packet_queue_peekpts(&ctx->output_audio_list);
    if (pkt->pts < cur_pts) {
        /* Older than the first packet in the list, so we will end up throwing it away */
        av_log(avctx, AV_LOG_WARNING, "Audio packet too old, discarding.  PTS=%ld first=%ld\n",
               pkt->pts, cur_pts);
    }

    remaining = sample_count;
    src_offset = 0;
    for (cur = ctx->output_audio_list.pkt_list.head; cur != NULL; cur = cur->next) {
        /* See if we should interleave into this packet */
        cur_pkt = &cur->pkt;
        if (((pkt->pts) >= cur_pkt->pts) &&
            (pkt->pts) < (cur->pkt.pts + ctx->audio_pkt_numsamples)) {
            unsigned int num_copy = remaining;
            unsigned int dst_offset = pkt->pts - cur->pkt.pts;

            /* Don't overflow dest buffer */
            if (num_copy > (ctx->audio_pkt_numsamples - dst_offset))
                num_copy = ctx->audio_pkt_numsamples - dst_offset;

            /* Yes, interleave */
            sample_offset = (dst_offset * ctx->channels + interleave_offset) * 2;
            for (unsigned int i = 0; i < num_copy; i++) {
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
            ret = av_new_packet(&pkt_new, ctx->audio_pkt_numsamples * ctx->channels * 2);
            if (ret != 0)
                goto done_unlock;
            memset(pkt_new.data, 0, ctx->audio_pkt_numsamples * ctx->channels * 2);
            pkt_new.pts = cur->pkt.pts + ctx->audio_pkt_numsamples;
            ff_decklink_packet_queue_put(&ctx->output_audio_list, &pkt_new);
        }
    }

    /* Stash the last PTS for the next call */
    ctx->audio_st_lastpts[pkt->stream_index] = pkt->pts;

done_unlock:
    pthread_mutex_unlock(&ctx->audio_mutex);

    if (st->codecpar->codec_id == AV_CODEC_ID_AC3)
        av_freep(&outbuf);

    return ret;
}

static int decklink_write_subtitle_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;

    ff_ccfifo_extractbytes(&ctx->cc_fifo, pkt->data, pkt->size);

    return 0;
}

static int decklink_write_data_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;

    if (ff_decklink_packet_queue_put(&ctx->vanc_queue, pkt) < 0) {
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
    int ret;

    ctx = (struct decklink_ctx *) av_mallocz(sizeof(struct decklink_ctx));
    if (!ctx)
        return AVERROR(ENOMEM);
    ctx->list_devices = cctx->list_devices;
    ctx->list_formats = cctx->list_formats;
    ctx->preroll      = cctx->preroll;
    ctx->duplex_mode  = cctx->duplex_mode;
    ctx->first_pts    = AV_NOPTS_VALUE;
    if (cctx->link > 0 && (unsigned int)cctx->link < FF_ARRAY_ELEMS(decklink_link_conf_map))
        ctx->link = decklink_link_conf_map[cctx->link];
    cctx->ctx = ctx;
#if CONFIG_LIBKLVANC
    if (klvanc_context_create(&ctx->vanc_ctx) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create VANC library context\n");
        return AVERROR(ENOMEM);
    }
    ctx->supports_vanc = 1;
#endif

    /* List available devices and exit. */
    if (ctx->list_devices) {
        ff_decklink_list_devices_legacy(avctx, 0, 1);
        return AVERROR_EXIT;
    }

    ret = ff_decklink_init_device(avctx, avctx->url);
    if (ret < 0)
        return ret;

    /* Get output device. */
    if (ctx->dl->QueryInterface(IID_IDeckLinkOutput, (void **) &ctx->dlo) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not open output device from '%s'\n",
               avctx->url);
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
        } else if (c->codec_type == AVMEDIA_TYPE_DATA ||
                   c->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            /* Do nothing (we initialize those streams later) */
        } else {
            av_log(avctx, AV_LOG_ERROR, "Unsupported stream type.\n");
            goto error;
        }
    }

    /* Reconfigure the data/subtitle stream clocks to match the video */
    for (n = 0; n < avctx->nb_streams; n++) {
        AVStream *st = avctx->streams[n];
        AVCodecParameters *c = st->codecpar;

        if(c->codec_type == AVMEDIA_TYPE_DATA ||
           c->codec_type == AVMEDIA_TYPE_SUBTITLE)
            avpriv_set_pts_info(st, 64, ctx->bmd_tb_num, ctx->bmd_tb_den);
    }

    /* Now that video has been setup and the time_base has been set for any
       data/subtitle streams, do the setup.  This ensures that any automatically
       inserted bitstream filters are initialized with the correct time base.  */
    for (n = 0; n < avctx->nb_streams; n++) {
        AVStream *st = avctx->streams[n];
        AVCodecParameters *c = st->codecpar;
        if (c->codec_type == AVMEDIA_TYPE_DATA) {
            if (decklink_setup_data(avctx, st))
                goto error;
        } else if (c->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            if (decklink_setup_subtitle(avctx, st))
                goto error;
        }
    }

    /* Set up the VANC queue for receiving packets that arrive separately from
       the video */
    ff_decklink_packet_queue_init(avctx, &ctx->vanc_queue, cctx->vanc_queue_size);

    ret = ff_ccfifo_init(&ctx->cc_fifo, av_make_q(ctx->bmd_tb_den, ctx->bmd_tb_num), avctx);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failure to setup CC FIFO queue\n");
        goto error;
    }

    if (ctx->audio > 0) {
        ctx->audio_st_lastpts = (int64_t *) av_malloc_array(avctx->nb_streams, sizeof(int64_t));
        if (ctx->audio_st_lastpts == NULL)
            goto error;
        if (decklink_enable_audio(avctx))
            goto error;
    }

    ltnlog_stat("VIDEOMODE", ctx->bmd_mode);
    ltnlog_stat("AUDIO STREAMCOUNT", ctx->audio);
    ltnlog_stat("AUDIO CHANNELCOUNT", ctx->channels);

    return 0;

error:
    ff_decklink_cleanup(avctx);
    return ret;
}

int ff_decklink_write_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    AVStream *st = avctx->streams[pkt->stream_index];
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    int ret = AVERROR(EIO);

    if (cctx->debug_level >= 4)
        av_log(avctx, AV_LOG_INFO, "%s called. Type=%s pts=%" PRId64 "\n", __func__,
               av_get_media_type_string(st->codecpar->codec_type), pkt->pts);

    if      (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        ret = decklink_write_video_packet(avctx, pkt);
    else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        ret = decklink_write_audio_packet(avctx, pkt);
    else if (st->codecpar->codec_type == AVMEDIA_TYPE_DATA)
        ret = decklink_write_data_packet(avctx, pkt);
    else if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
        ret = decklink_write_subtitle_packet(avctx, pkt);

    if (cctx->debug_level >= 4)
        av_log(avctx, AV_LOG_INFO, "%s returning.  Type=%s\n", __func__,
               av_get_media_type_string(st->codecpar->codec_type));

    return ret;
}

int ff_decklink_list_output_devices(AVFormatContext *avctx, struct AVDeviceInfoList *device_list)
{
    return ff_decklink_list_devices(avctx, device_list, 0, 1);
}

} /* extern "C" */
