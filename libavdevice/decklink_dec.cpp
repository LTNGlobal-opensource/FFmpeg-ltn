/*
 * Blackmagic DeckLink input
 * Copyright (c) 2013-2014 Luca Barbato, Deti Fliegl
 * Copyright (c) 2014 Rafaël Carré
 * Copyright (c) 2017 Akamai Technologies, Inc.
 * Copyright (c) 2017 LTN Global Communications, Inc.
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

/* Include internal.h first to avoid conflict between winsock.h (used by
 * DeckLink headers) and winsock2.h (used by libavformat) in MSVC++ builds */
extern "C" {
#include "libavformat/internal.h"
}

#include <DeckLinkAPI.h>

extern "C" {
#include "config.h"
#include "libavformat/avformat.h"
#include "libavutil/avassert.h"
#include "libavutil/avutil.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/time.h"
#include "libavutil/mathematics.h"
#include "libavutil/reverse.h"
#include "avdevice.h"
#if CONFIG_LIBZVBI
#include <libzvbi.h>
#endif
}

#include "decklink_common.h"
#include "decklink_dec.h"

#define MAX_WIDTH_VANC 1920

typedef struct VANCLineNumber {
    BMDDisplayMode mode;
    int vanc_start;
    int field0_vanc_end;
    int field1_vanc_start;
    int vanc_end;
} VANCLineNumber;

/* These VANC line numbers need not be very accurate. In any case
 * GetBufferForVerticalBlankingLine() will return an error when invalid
 * ancillary line number was requested. We just need to make sure that the
 * entire VANC region is covered, while making sure we don't decode VANC of
 * another source during switching*/
static VANCLineNumber vanc_line_numbers[] = {
    /* SD Modes */

    {bmdModeNTSC, 11, 19, 274, 282},
    {bmdModeNTSC2398, 11, 19, 274, 282},
    {bmdModePAL, 7, 22, 320, 335},
    {bmdModeNTSCp, 11, -1, -1, 39},
    {bmdModePALp, 7, -1, -1, 45},

    /* HD 1080 Modes */

    {bmdModeHD1080p2398, 8, -1, -1, 42},
    {bmdModeHD1080p24, 8, -1, -1, 42},
    {bmdModeHD1080p25, 8, -1, -1, 42},
    {bmdModeHD1080p2997, 8, -1, -1, 42},
    {bmdModeHD1080p30, 8, -1, -1, 42},
    {bmdModeHD1080i50, 8, 20, 570, 585},
    {bmdModeHD1080i5994, 8, 20, 570, 585},
    {bmdModeHD1080i6000, 8, 20, 570, 585},
    {bmdModeHD1080p50, 8, -1, -1, 42},
    {bmdModeHD1080p5994, 8, -1, -1, 42},
    {bmdModeHD1080p6000, 8, -1, -1, 42},

     /* HD 720 Modes */

    {bmdModeHD720p50, 8, -1, -1, 26},
    {bmdModeHD720p5994, 8, -1, -1, 26},
    {bmdModeHD720p60, 8, -1, -1, 26},

    /* For all other modes, for which we don't support VANC */
    {bmdModeUnknown, 0, -1, -1, -1}
};

static int get_vanc_line_idx(BMDDisplayMode mode)
{
    unsigned int i;
    for (i = 0; i < FF_ARRAY_ELEMS(vanc_line_numbers); i++) {
        if (mode == vanc_line_numbers[i].mode)
            return i;
    }
    /* Return the VANC idx for Unknown mode */
    return i - 1;
}

static inline void clear_parity_bits(uint16_t *buf, int len) {
    int i;
    for (i = 0; i < len; i++)
        buf[i] &= 0xff;
}

static int check_vanc_parity_checksum(uint16_t *buf, int len, uint16_t checksum) {
    int i;
    uint16_t vanc_sum = 0;
    for (i = 3; i < len - 1; i++) {
        uint16_t v = buf[i];
        int np = v >> 8;
        int p = av_parity(v & 0xff);
        if ((!!p ^ !!(v & 0x100)) || (np != 1 && np != 2)) {
            // Parity check failed
            return -1;
        }
        vanc_sum += v;
    }
    vanc_sum &= 0x1ff;
    vanc_sum |= ((~vanc_sum & 0x100) << 1);
    if (checksum != vanc_sum) {
        // Checksum verification failed
        return -1;
    }
    return 0;
}

/* The 10-bit VANC data is packed in V210, we only need the luma component. */
static void extract_luma_from_v210(uint16_t *dst, const uint8_t *src, int width)
{
    int i;
    for (i = 0; i < width / 3; i += 3) {
        *dst++ = (src[1] >> 2) + ((src[2] & 15) << 6);
        *dst++ =  src[4]       + ((src[5] &  3) << 8);
        *dst++ = (src[6] >> 4) + ((src[7] & 63) << 4);
        src += 8;
    }
}

static uint8_t calc_parity_and_line_offset(int line)
{
    uint8_t ret = (line < 313) << 5;
    if (line >= 7 && line <= 22)
        ret += line;
    if (line >= 320 && line <= 335)
        ret += (line - 313);
    return ret;
}

static void fill_data_unit_head(int line, uint8_t *tgt)
{
    tgt[0] = 0x02; // data_unit_id
    tgt[1] = 0x2c; // data_unit_length
    tgt[2] = calc_parity_and_line_offset(line); // field_parity, line_offset
    tgt[3] = 0xe4; // framing code
}

#if CONFIG_LIBZVBI
static uint8_t* teletext_data_unit_from_vbi_data(int line, uint8_t *src, uint8_t *tgt, vbi_pixfmt fmt)
{
    vbi_bit_slicer slicer;

    vbi_bit_slicer_init(&slicer, 720, 13500000, 6937500, 6937500, 0x00aaaae4, 0xffff, 18, 6, 42 * 8, VBI_MODULATION_NRZ_MSB, fmt);

    if (vbi_bit_slice(&slicer, src, tgt + 4) == FALSE)
        return tgt;

    fill_data_unit_head(line, tgt);

    return tgt + 46;
}

static uint8_t* teletext_data_unit_from_vbi_data_10bit(int line, uint8_t *src, uint8_t *tgt)
{
    uint8_t y[720];
    uint8_t *py = y;
    uint8_t *pend = y + 720;
    /* The 10-bit VBI data is packed in V210, but libzvbi only supports 8-bit,
     * so we extract the 8 MSBs of the luma component, that is enough for
     * teletext bit slicing. */
    while (py < pend) {
        *py++ = (src[1] >> 4) + ((src[2] & 15) << 4);
        *py++ = (src[4] >> 2) + ((src[5] & 3 ) << 6);
        *py++ = (src[6] >> 6) + ((src[7] & 63) << 2);
        src += 8;
    }
    return teletext_data_unit_from_vbi_data(line, y, tgt, VBI_PIXFMT_YUV420);
}
#endif

static uint8_t* teletext_data_unit_from_op47_vbi_packet(int line, uint16_t *py, uint8_t *tgt)
{
    int i;

    if (py[0] != 0x255 || py[1] != 0x255 || py[2] != 0x227)
        return tgt;

    fill_data_unit_head(line, tgt);

    py += 3;
    tgt += 4;

    for (i = 0; i < 42; i++)
       *tgt++ = ff_reverse[py[i] & 255];

    return tgt;
}

static int linemask_matches(int line, int64_t mask)
{
    int shift = -1;
    if (line >= 6 && line <= 22)
        shift = line - 6;
    if (line >= 318 && line <= 335)
        shift = line - 318 + 17;
    return shift >= 0 && ((1ULL << shift) & mask);
}

static uint8_t* teletext_data_unit_from_op47_data(uint16_t *py, uint16_t *pend, uint8_t *tgt, int64_t wanted_lines)
{
    if (py < pend - 9) {
        if (py[0] == 0x151 && py[1] == 0x115 && py[3] == 0x102) {       // identifier, identifier, format code for WST teletext
            uint16_t *descriptors = py + 4;
            int i;
            py += 9;
            for (i = 0; i < 5 && py < pend - 45; i++, py += 45) {
                int line = (descriptors[i] & 31) + (!(descriptors[i] & 128)) * 313;
                if (line && linemask_matches(line, wanted_lines))
                    tgt = teletext_data_unit_from_op47_vbi_packet(line, py, tgt);
            }
        }
    }
    return tgt;
}

static uint8_t* teletext_data_unit_from_ancillary_packet(uint16_t *py, uint16_t *pend, uint8_t *tgt, int64_t wanted_lines, int allow_multipacket)
{
    uint16_t did = py[0];                                               // data id
    uint16_t sdid = py[1];                                              // secondary data id
    uint16_t dc = py[2] & 255;                                          // data count
    py += 3;
    pend = FFMIN(pend, py + dc);
    if (did == 0x143 && sdid == 0x102) {                                // subtitle distribution packet
        tgt = teletext_data_unit_from_op47_data(py, pend, tgt, wanted_lines);
    } else if (allow_multipacket && did == 0x143 && sdid == 0x203) {    // VANC multipacket
        py += 2;                                                        // priority, line/field
        while (py < pend - 3) {
            tgt = teletext_data_unit_from_ancillary_packet(py, pend, tgt, wanted_lines, 0);
            py += 4 + (py[2] & 255);                                    // ndid, nsdid, ndc, line/field
        }
    }
    return tgt;
}

uint8_t *vanc_to_cc(AVFormatContext *avctx, uint16_t *buf, size_t words,
    unsigned &cc_count)
{
    size_t i, len = (buf[5] & 0xff) + 6 + 1;
    uint8_t cdp_sum, rate;
    uint16_t hdr, ftr;
    uint8_t *cc;
    uint16_t *cdp = &buf[6]; // CDP follows
    if (cdp[0] != 0x96 || cdp[1] != 0x69) {
        av_log(avctx, AV_LOG_WARNING, "Invalid CDP header 0x%.2x 0x%.2x\n", cdp[0], cdp[1]);
        return NULL;
    }

    len -= 7; // remove VANC header and checksum

    if (cdp[2] != len) {
        av_log(avctx, AV_LOG_WARNING, "CDP len %d != %zu\n", cdp[2], len);
        return NULL;
    }

    cdp_sum = 0;
    for (i = 0; i < len - 1; i++)
        cdp_sum += cdp[i];
    cdp_sum = cdp_sum ? 256 - cdp_sum : 0;
    if (cdp[len - 1] != cdp_sum) {
        av_log(avctx, AV_LOG_WARNING, "CDP checksum invalid 0x%.4x != 0x%.4x\n", cdp_sum, cdp[len-1]);
        return NULL;
    }

    rate = cdp[3];
    if (!(rate & 0x0f)) {
        av_log(avctx, AV_LOG_WARNING, "CDP frame rate invalid (0x%.2x)\n", rate);
        return NULL;
    }
    rate >>= 4;
    if (rate > 8) {
        av_log(avctx, AV_LOG_WARNING, "CDP frame rate invalid (0x%.2x)\n", rate);
        return NULL;
    }

    if (!(cdp[4] & 0x43)) /* ccdata_present | caption_service_active | reserved */ {
        av_log(avctx, AV_LOG_WARNING, "CDP flags invalid (0x%.2x)\n", cdp[4]);
        return NULL;
    }

    hdr = (cdp[5] << 8) | cdp[6];
    if (cdp[7] != 0x72) /* ccdata_id */ {
        av_log(avctx, AV_LOG_WARNING, "Invalid ccdata_id 0x%.2x\n", cdp[7]);
        return NULL;
    }

    cc_count = cdp[8];
    if (!(cc_count & 0xe0)) {
        av_log(avctx, AV_LOG_WARNING, "Invalid cc_count 0x%.2x\n", cc_count);
        return NULL;
    }

    cc_count &= 0x1f;
    if ((len - 13) < cc_count * 3) {
        av_log(avctx, AV_LOG_WARNING, "Invalid cc_count %d (> %zu)\n", cc_count * 3, len - 13);
        return NULL;
    }

    if (cdp[len - 4] != 0x74) /* footer id */ {
        av_log(avctx, AV_LOG_WARNING, "Invalid footer id 0x%.2x\n", cdp[len-4]);
        return NULL;
    }

    ftr = (cdp[len - 3] << 8) | cdp[len - 2];
    if (ftr != hdr) {
        av_log(avctx, AV_LOG_WARNING, "Header 0x%.4x != Footer 0x%.4x\n", hdr, ftr);
        return NULL;
    }

    cc = (uint8_t *)av_malloc(cc_count * 3);
    if (cc == NULL) {
        av_log(avctx, AV_LOG_WARNING, "CC - av_malloc failed for cc_count = %d\n", cc_count);
        return NULL;
    }

    for (size_t i = 0; i < cc_count; i++) {
        cc[3*i + 0] = cdp[9 + 3*i+0] /* & 3 */;
        cc[3*i + 1] = cdp[9 + 3*i+1];
        cc[3*i + 2] = cdp[9 + 3*i+2];
    }

    cc_count *= 3;
    return cc;
}

uint8_t *get_metadata(AVFormatContext *avctx, uint16_t *buf, size_t width,
                      uint8_t *tgt, size_t tgt_size, AVPacket *pkt)
{
    decklink_cctx *cctx = (struct decklink_cctx *) avctx->priv_data;
    uint16_t *max_buf = buf + width;

    while (buf < max_buf - 6) {
        int len;
        uint16_t did = buf[3] & 0xFF;                                  // data id
        uint16_t sdid = buf[4] & 0xFF;                                 // secondary data id
        /* Check for VANC header */
        if (buf[0] != 0 || buf[1] != 0x3ff || buf[2] != 0x3ff) {
            return tgt;
        }

        len = (buf[5] & 0xff) + 6 + 1;
        if (len > max_buf - buf) {
            av_log(avctx, AV_LOG_WARNING, "Data Count (%d) > data left (%zu)\n",
                    len, max_buf - buf);
            return tgt;
        }

        if (did == 0x43 && (sdid == 0x02 || sdid == 0x03) && cctx->teletext_lines &&
            width == 1920 && tgt_size >= 1920) {
            if (check_vanc_parity_checksum(buf, len, buf[len - 1]) < 0) {
                av_log(avctx, AV_LOG_WARNING, "VANC parity or checksum incorrect\n");
                goto skip_packet;
            }
            tgt = teletext_data_unit_from_ancillary_packet(buf + 3, buf + len, tgt, cctx->teletext_lines, 1);
        } else if (did == 0x61 && sdid == 0x01) {
            unsigned int data_len;
            uint8_t *data;
            if (check_vanc_parity_checksum(buf, len, buf[len - 1]) < 0) {
                av_log(avctx, AV_LOG_WARNING, "VANC parity or checksum incorrect\n");
                goto skip_packet;
            }
            clear_parity_bits(buf, len);
            data = vanc_to_cc(avctx, buf, width, data_len);
            if (data) {
                if (av_packet_add_side_data(pkt, AV_PKT_DATA_A53_CC, data, data_len) < 0)
                    av_free(data);
            }
        } else {
            av_log(avctx, AV_LOG_DEBUG, "Unknown meta data DID = 0x%.2x SDID = 0x%.2x\n",
                    did, sdid);
        }
skip_packet:
        buf += len;
    }

    return tgt;
}

static void avpacket_queue_init(AVFormatContext *avctx, AVPacketQueue *q)
{
    struct decklink_cctx *ctx = (struct decklink_cctx *)avctx->priv_data;
    memset(q, 0, sizeof(AVPacketQueue));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    q->avctx = avctx;
    q->max_q_size = ctx->queue_size;
}

static void avpacket_queue_flush(AVPacketQueue *q)
{
    AVPacketList *pkt, *pkt1;

    pthread_mutex_lock(&q->mutex);
    for (pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt   = NULL;
    q->first_pkt  = NULL;
    q->nb_packets = 0;
    q->size       = 0;
    pthread_mutex_unlock(&q->mutex);
}

static void avpacket_queue_end(AVPacketQueue *q)
{
    avpacket_queue_flush(q);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static unsigned long long avpacket_queue_size(AVPacketQueue *q)
{
    unsigned long long size;
    pthread_mutex_lock(&q->mutex);
    size = q->size;
    pthread_mutex_unlock(&q->mutex);
    return size;
}

static int avpacket_queue_put(AVPacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;
    int ret;

    // Drop Packet if queue size is > maximum queue size
    if (avpacket_queue_size(q) > (uint64_t)q->max_q_size) {
        av_log(q->avctx, AV_LOG_WARNING,  "Decklink input buffer overrun!\n");
        return -1;
    }

    pkt1 = (AVPacketList *)av_mallocz(sizeof(AVPacketList));
    if (!pkt1) {
        return -1;
    }
    ret = av_packet_ref(&pkt1->pkt, pkt);
    av_packet_unref(pkt);
    if (ret < 0) {
        av_free(pkt1);
        return -1;
    }
    pkt1->next = NULL;

    pthread_mutex_lock(&q->mutex);

    if (!q->last_pkt) {
        q->first_pkt = pkt1;
    } else {
        q->last_pkt->next = pkt1;
    }

    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);

    pthread_cond_signal(&q->cond);

    pthread_mutex_unlock(&q->mutex);
    return 0;
}

static int avpacket_queue_get(AVPacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    pthread_mutex_lock(&q->mutex);

    for (;; ) {
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt) {
                q->last_pkt = NULL;
            }
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            *pkt     = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            pthread_cond_wait(&q->cond, &q->mutex);
        }
    }
    pthread_mutex_unlock(&q->mutex);
    return ret;
}

class decklink_input_callback : public IDeckLinkInputCallback
{
public:
        decklink_input_callback(AVFormatContext *_avctx);
        ~decklink_input_callback();

        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) { return E_NOINTERFACE; }
        virtual ULONG STDMETHODCALLTYPE AddRef(void);
        virtual ULONG STDMETHODCALLTYPE  Release(void);
        virtual HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode*, BMDDetectedVideoInputFormatFlags);
        virtual HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame*, IDeckLinkAudioInputPacket*);

private:
        ULONG           m_refCount;
        pthread_mutex_t m_mutex;
        AVFormatContext *avctx;
        decklink_ctx    *ctx;
        int no_video;
        int64_t initial_video_pts;
        int64_t initial_audio_pts;
};

decklink_input_callback::decklink_input_callback(AVFormatContext *_avctx) : m_refCount(0)
{
    avctx = _avctx;
    decklink_cctx       *cctx = (struct decklink_cctx *)avctx->priv_data;
    ctx = (struct decklink_ctx *)cctx->ctx;
    no_video = 0;
    initial_audio_pts = initial_video_pts = AV_NOPTS_VALUE;
    pthread_mutex_init(&m_mutex, NULL);
}

decklink_input_callback::~decklink_input_callback()
{
    pthread_mutex_destroy(&m_mutex);
}

ULONG decklink_input_callback::AddRef(void)
{
    pthread_mutex_lock(&m_mutex);
    m_refCount++;
    pthread_mutex_unlock(&m_mutex);

    return (ULONG)m_refCount;
}

ULONG decklink_input_callback::Release(void)
{
    pthread_mutex_lock(&m_mutex);
    m_refCount--;
    pthread_mutex_unlock(&m_mutex);

    if (m_refCount == 0) {
        delete this;
        return 0;
    }

    return (ULONG)m_refCount;
}

static int64_t get_pkt_pts(IDeckLinkVideoInputFrame *videoFrame,
                           IDeckLinkAudioInputPacket *audioFrame,
                           int64_t wallclock,
                           DecklinkPtsSource pts_src,
                           AVRational time_base, int64_t *initial_pts)
{
    int64_t pts = AV_NOPTS_VALUE;
    BMDTimeValue bmd_pts;
    BMDTimeValue bmd_duration;
    HRESULT res = E_INVALIDARG;
    switch (pts_src) {
        case PTS_SRC_AUDIO:
            if (audioFrame)
                res = audioFrame->GetPacketTime(&bmd_pts, time_base.den);
            break;
        case PTS_SRC_VIDEO:
            if (videoFrame)
                res = videoFrame->GetStreamTime(&bmd_pts, &bmd_duration, time_base.den);
            break;
        case PTS_SRC_REFERENCE:
            if (videoFrame)
                res = videoFrame->GetHardwareReferenceTimestamp(time_base.den, &bmd_pts, &bmd_duration);
            break;
        case PTS_SRC_WALLCLOCK:
        {
            /* MSVC does not support compound literals like AV_TIME_BASE_Q
             * in C++ code (compiler error C4576) */
            AVRational timebase;
            timebase.num = 1;
            timebase.den = AV_TIME_BASE;
            pts = av_rescale_q(wallclock, timebase, time_base);
            break;
        }
    }
    if (res == S_OK)
        pts = bmd_pts / time_base.num;

    if (pts != AV_NOPTS_VALUE && *initial_pts == AV_NOPTS_VALUE)
        *initial_pts = pts;
    if (*initial_pts != AV_NOPTS_VALUE)
        pts -= *initial_pts;

    return pts;
}

static int setup_audio(AVFormatContext *avctx)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVStream *st;
    int ret = 0;

    if (cctx->audio_mode == AUDIO_MODE_DISCRETE) {
        st = avformat_new_stream(avctx, NULL);
        if (!st) {
            av_log(avctx, AV_LOG_ERROR, "Cannot add stream\n");
            ret = AVERROR(ENOMEM);
            goto error;
        }
        st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
        st->codecpar->codec_id    = AV_CODEC_ID_PCM_S16LE;
        st->codecpar->sample_rate = bmdAudioSampleRate48kHz;
        st->codecpar->channels    = cctx->audio_channels;
        avpriv_set_pts_info(st, 64, 1, 1000000);  /* 64 bits pts in us */
        ctx->audio_st[0] = st;
        ctx->num_audio_streams++;
    } else {
        for (int i = 0; i < ctx->max_audio_channels / 2; i++) {
            st = avformat_new_stream(avctx, NULL);
            if (!st) {
                av_log(avctx, AV_LOG_ERROR, "Cannot add stream %d\n", i);
                ret = AVERROR(ENOMEM);
                goto error;
            }
            st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
            st->codecpar->codec_id    = AV_CODEC_ID_PCM_S16LE;
            st->codecpar->sample_rate = bmdAudioSampleRate48kHz;
            st->codecpar->channels    = 2;
            avpriv_set_pts_info(st, 64, 1, 1000000);  /* 64 bits pts in us */
            ctx->audio_st[i] = st;
            ctx->num_audio_streams++;
        }
        cctx->audio_channels = ctx->max_audio_channels;
    }

error:
    return ret;
}

#if CONFIG_LIBKLVANC
/* VANC Callbacks */
struct vanc_cb_ctx {
    AVFormatContext *avctx;
    AVPacket *pkt;
};
static int cb_AFD(void *callback_context, struct klvanc_context_s *ctx,
                  struct klvanc_packet_afd_s *pkt)
{
    struct vanc_cb_ctx *cb_ctx = (struct vanc_cb_ctx *)callback_context;
    uint8_t *afd;

    afd = (uint8_t *)av_malloc(1);
    if (afd == NULL)
        return AVERROR(ENOMEM);

    afd[0] = pkt->hdr.payload[0] >> 3;
    if (av_packet_add_side_data(cb_ctx->pkt, AV_PKT_DATA_AFD, afd, 1) < 0)
        av_free(afd);

    return 0;
}

static int cb_EIA_708B(void *callback_context, struct klvanc_context_s *ctx,
                       struct klvanc_packet_eia_708b_s *pkt)
{
    struct vanc_cb_ctx *cb_ctx = (struct vanc_cb_ctx *)callback_context;
    decklink_cctx *cctx = (struct decklink_cctx *)cb_ctx->avctx->priv_data;
    uint16_t expected_cdp;
    uint8_t *cc;

    if (!pkt->checksum_valid)
        return 0;

    if (!pkt->header.ccdata_present)
        return 0;

    expected_cdp = cctx->last_cdp_count + 1;
    cctx->last_cdp_count = pkt->header.cdp_hdr_sequence_cntr;
    if (pkt->header.cdp_hdr_sequence_cntr != expected_cdp) {
        av_log(cb_ctx->avctx, AV_LOG_DEBUG,
               "CDP counter inconsistent.  Received=0x%04x Expected=%04x\n",
               pkt->header.cdp_hdr_sequence_cntr, expected_cdp);
        return 0;
    }

    cc = (uint8_t *)av_malloc(pkt->ccdata.cc_count * 3);
    if (cc == NULL)
        return AVERROR(ENOMEM);

    for (int i = 0; i < pkt->ccdata.cc_count; i++) {
        cc[3*i] = 0xf8 | (pkt->ccdata.cc[i].cc_valid ? 0x04 : 0x00) |
                  (pkt->ccdata.cc[i].cc_type & 0x03);
        cc[3*i+1] = pkt->ccdata.cc[i].cc_data[0];
        cc[3*i+2] = pkt->ccdata.cc[i].cc_data[1];
    }

    if (av_packet_add_side_data(cb_ctx->pkt, AV_PKT_DATA_A53_CC, cc, pkt->ccdata.cc_count * 3) < 0)
        av_free(cc);

    return 0;
}

static struct klvanc_callbacks_s callbacks =
{
    .afd               = cb_AFD,
    .eia_708b          = cb_EIA_708B,
    .eia_608           = NULL,
    .scte_104          = NULL,
    .all               = NULL,
    .kl_i64le_counter  = NULL,
};
/* End: VANC Callbacks */

/* Take one line of V210 from VANC, colorspace convert and feed it to the
 * VANC parser. We'll expect our VANC message callbacks to happen on this
 * same calling thread.
 */
static void klvanc_handle_line(AVFormatContext *avctx, struct klvanc_context_s *vanc_ctx,
                               unsigned char *buf, unsigned int uiWidth, unsigned int lineNr,
                               AVPacket *pkt)
{
    decklink_cctx *decklink_ctx = (struct decklink_cctx *)avctx->priv_data;

    /* Convert the vanc line from V210 to CrCB422, then vanc parse it */

    /* We need two kinds of type pointers into the source vbi buffer */
    /* TODO: What the hell is this, two ptrs? */
    const uint32_t *src = (const uint32_t *)buf;

    /* Convert Blackmagic pixel format to nv20.
     * src pointer gets mangled during conversion, hence we need its own
     * ptr instead of passing vbiBufferPtr.
     * decoded_words should be atleast 2 * uiWidth.
     */
    uint16_t decoded_words[16384];

    /* On output each pixel will be decomposed into three 16-bit words (one for Y, U, V) */
    memset(&decoded_words[0], 0, sizeof(decoded_words));
    uint16_t *p_anc = decoded_words;
    if (klvanc_v210_line_to_nv20_c(src, p_anc, sizeof(decoded_words), (uiWidth / 6) * 6) < 0)
        return;

    if (decklink_ctx->vanc_ctx) {
        struct vanc_cb_ctx cb_ctx = {
            .avctx = avctx,
            .pkt = pkt
        };
        vanc_ctx->callback_context = &cb_ctx;
        int ret = klvanc_packet_parse(vanc_ctx, lineNr, decoded_words, sizeof(decoded_words) / (sizeof(unsigned short)));
        if (ret < 0) {
            /* No VANC on this line */
        }
    }
}
#endif

HRESULT decklink_input_callback::VideoInputFrameArrived(
    IDeckLinkVideoInputFrame *videoFrame, IDeckLinkAudioInputPacket *audioFrame)
{
    decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    void *frameBytes;
    void *audioFrameBytes;
    BMDTimeValue frameTime;
    BMDTimeValue frameDuration;
    int64_t wallclock = 0;

    ctx->frameCount++;
    if (ctx->audio_pts_source == PTS_SRC_WALLCLOCK || ctx->video_pts_source == PTS_SRC_WALLCLOCK)
        wallclock = av_gettime_relative();

    // Handle Video Frame
    if (videoFrame) {
        AVPacket pkt;
        av_init_packet(&pkt);
        if (ctx->frameCount % 25 == 0) {
            unsigned long long qsize = avpacket_queue_size(&ctx->queue);
            av_log(avctx, AV_LOG_DEBUG,
                    "Frame received (#%lu) - Valid (%liB) - QSize %fMB\n",
                    ctx->frameCount,
                    videoFrame->GetRowBytes() * videoFrame->GetHeight(),
                    (double)qsize / 1024 / 1024);
        }

        videoFrame->GetBytes(&frameBytes);
        videoFrame->GetStreamTime(&frameTime, &frameDuration,
                                  ctx->video_st->time_base.den);

        if (videoFrame->GetFlags() & bmdFrameHasNoInputSource) {
            if (ctx->draw_bars && videoFrame->GetPixelFormat() == bmdFormat8BitYUV) {
                unsigned bars[8] = {
                    0xEA80EA80, 0xD292D210, 0xA910A9A5, 0x90229035,
                    0x6ADD6ACA, 0x51EF515A, 0x286D28EF, 0x10801080 };
                int width  = videoFrame->GetWidth();
                int height = videoFrame->GetHeight();
                unsigned *p = (unsigned *)frameBytes;

                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < width; x += 2)
                        *p++ = bars[(x * 8) / width];
                }
            }

            if (!no_video) {
                av_log(avctx, AV_LOG_WARNING, "Frame received (#%lu) - No input signal detected "
                        "- Frames dropped %u\n", ctx->frameCount, ++ctx->dropped);
            }
            no_video = 1;
        } else {
            if (no_video) {
                av_log(avctx, AV_LOG_WARNING, "Frame received (#%lu) - Input returned "
                        "- Frames dropped %u\n", ctx->frameCount, ++ctx->dropped);
            }
            no_video = 0;
        }

        pkt.pts = get_pkt_pts(videoFrame, audioFrame, wallclock, ctx->video_pts_source, ctx->video_st->time_base, &initial_video_pts);
        pkt.dts = pkt.pts;

        pkt.duration = frameDuration;
        //To be made sure it still applies
        pkt.flags       |= AV_PKT_FLAG_KEY;
        pkt.stream_index = ctx->video_st->index;
        pkt.data         = (uint8_t *)frameBytes;
        pkt.size         = videoFrame->GetRowBytes() *
                           videoFrame->GetHeight();
        //fprintf(stderr,"Video Frame size %d ts %d\n", pkt.size, pkt.pts);

        if (!no_video) {
            IDeckLinkVideoFrameAncillary *vanc;
            AVPacket txt_pkt;
            uint8_t txt_buf0[3531]; // 35 * 46 bytes decoded teletext lines + 1 byte data_identifier + 1920 bytes OP47 decode buffer
            uint8_t *txt_buf = txt_buf0;

            if (videoFrame->GetAncillaryData(&vanc) == S_OK) {
                int i;
                int64_t line_mask = 1;
                BMDPixelFormat vanc_format = vanc->GetPixelFormat();
                txt_buf[0] = 0x10;    // data_identifier - EBU_data
                txt_buf++;
#if CONFIG_LIBZVBI
                if (ctx->bmd_mode == bmdModePAL && ctx->teletext_lines &&
                    (vanc_format == bmdFormat8BitYUV || vanc_format == bmdFormat10BitYUV)) {
                    av_assert0(videoFrame->GetWidth() == 720);
                    for (i = 6; i < 336; i++, line_mask <<= 1) {
                        uint8_t *buf;
                        if ((ctx->teletext_lines & line_mask) && vanc->GetBufferForVerticalBlankingLine(i, (void**)&buf) == S_OK) {
                            if (vanc_format == bmdFormat8BitYUV)
                                txt_buf = teletext_data_unit_from_vbi_data(i, buf, txt_buf, VBI_PIXFMT_UYVY);
                            else
                                txt_buf = teletext_data_unit_from_vbi_data_10bit(i, buf, txt_buf);
                        }
                        if (i == 22)
                            i = 317;
                    }
                }
#endif
                if (vanc_format == bmdFormat10BitYUV && videoFrame->GetWidth() <= MAX_WIDTH_VANC) {
                    int idx = get_vanc_line_idx(ctx->bmd_mode);
                    for (i = vanc_line_numbers[idx].vanc_start; i <= vanc_line_numbers[idx].vanc_end; i++) {
                        uint8_t *buf;
                        if (vanc->GetBufferForVerticalBlankingLine(i, (void**)&buf) == S_OK) {
#if CONFIG_LIBKLVANC
                            klvanc_handle_line(avctx, cctx->vanc_ctx,
                                               buf, videoFrame->GetWidth(), i, &pkt);
#else
                            uint16_t luma_vanc[MAX_WIDTH_VANC];
                            extract_luma_from_v210(luma_vanc, buf, videoFrame->GetWidth());
                            txt_buf = get_metadata(avctx, luma_vanc, videoFrame->GetWidth(),
                                                   txt_buf, sizeof(txt_buf0) - (txt_buf - txt_buf0), &pkt);
#endif
                        }
                        if (i == vanc_line_numbers[idx].field0_vanc_end)
                            i = vanc_line_numbers[idx].field1_vanc_start - 1;
                    }
                }

                vanc->Release();
                if (txt_buf - txt_buf0 > 1) {
                    int stuffing_units = (4 - ((45 + txt_buf - txt_buf0) / 46) % 4) % 4;
                    while (stuffing_units--) {
                        memset(txt_buf, 0xff, 46);
                        txt_buf[1] = 0x2c; // data_unit_length
                        txt_buf += 46;
                    }
                    av_init_packet(&txt_pkt);
                    txt_pkt.pts = pkt.pts;
                    txt_pkt.dts = pkt.dts;
                    txt_pkt.stream_index = ctx->teletext_st->index;
                    txt_pkt.data = txt_buf0;
                    txt_pkt.size = txt_buf - txt_buf0;
                    if (avpacket_queue_put(&ctx->queue, &txt_pkt) < 0) {
                        ++ctx->dropped;
                    }
                }
            }
        }

        if (avpacket_queue_put(&ctx->queue, &pkt) < 0) {
            ++ctx->dropped;
        }
    }

    // Handle Audio Frame
    if (audioFrame) {
        audioFrame->GetBytes(&audioFrameBytes);

        if (cctx->audio_mode == AUDIO_MODE_DISCRETE) {
            AVPacket pkt;
            BMDTimeValue audio_pts;
            av_init_packet(&pkt);

            //hack among hacks
            pkt.size = audioFrame->GetSampleFrameCount() * ctx->audio_st[0]->codecpar->channels * (16 / 8);
            audioFrame->GetBytes(&audioFrameBytes);
            audioFrame->GetPacketTime(&audio_pts, ctx->audio_st[0]->time_base.den);
            pkt.pts = get_pkt_pts(videoFrame, audioFrame, wallclock, ctx->audio_pts_source, ctx->audio_st[0]->time_base, &initial_audio_pts);
            pkt.dts = pkt.pts;

            pkt.flags       |= AV_PKT_FLAG_KEY;
            pkt.stream_index = ctx->audio_st[0]->index;
            pkt.data         = (uint8_t *)audioFrameBytes;

            if (avpacket_queue_put(&ctx->queue, &pkt) < 0) {
                ++ctx->dropped;
            }
        } else {
            /* Need to deinterleave audio */
            int audio_offset = 0;
            int audio_stride = cctx->audio_channels * 2; /* FIXME: assumes 16-bit samples */
            for (int i = 0; i < ctx->num_audio_streams; i++) {
                int sample_size = ctx->audio_st[i]->codecpar->channels *
                                  ctx->audio_st[i]->codecpar->bits_per_coded_sample / 8;
                AVPacket pkt;
                int ret = av_new_packet(&pkt, audioFrame->GetSampleFrameCount() * sample_size);
                if (ret != 0)
                    continue;

                pkt.pts = get_pkt_pts(videoFrame, audioFrame, wallclock, ctx->audio_pts_source,
                                      ctx->audio_st[i]->time_base, &initial_audio_pts);
                pkt.dts          = pkt.pts;
                pkt.flags       |= AV_PKT_FLAG_KEY;
                pkt.stream_index = ctx->audio_st[i]->index;

                uint8_t *audio_in = ((uint8_t *) audioFrameBytes) + audio_offset;
                for (int x = 0; x < pkt.size; x += sample_size) {
                    memcpy(&pkt.data[x], audio_in, sample_size);
                    audio_in += audio_stride;
                }

                if (avpacket_queue_put(&ctx->queue, &pkt) < 0)
                    ++ctx->dropped;

                av_packet_unref(&pkt);
                audio_offset += sample_size;
            }
        }
    }

    return S_OK;
}

HRESULT decklink_input_callback::VideoInputFormatChanged(
    BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *mode,
    BMDDetectedVideoInputFormatFlags)
{
    return S_OK;
}

static HRESULT decklink_start_input(AVFormatContext *avctx)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;

    ctx->input_callback = new decklink_input_callback(avctx);
    ctx->dli->SetCallback(ctx->input_callback);
    return ctx->dli->StartStreams();
}

extern "C" {

av_cold int ff_decklink_read_close(AVFormatContext *avctx)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;

    if (ctx->capture_started) {
        ctx->dli->StopStreams();
        ctx->dli->DisableVideoInput();
        ctx->dli->DisableAudioInput();
    }

    ff_decklink_cleanup(avctx);
    avpacket_queue_end(&ctx->queue);

    av_freep(&cctx->ctx);

    return 0;
}

av_cold int ff_decklink_read_header(AVFormatContext *avctx)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx;
    AVStream *st;
    HRESULT result;
    char fname[1024];
    char *tmp;
    int mode_num = 0;
    int ret;

    ctx = (struct decklink_ctx *) av_mallocz(sizeof(struct decklink_ctx));
    if (!ctx)
        return AVERROR(ENOMEM);
    ctx->list_devices = cctx->list_devices;
    ctx->list_formats = cctx->list_formats;
    ctx->teletext_lines = cctx->teletext_lines;
    ctx->preroll      = cctx->preroll;
    ctx->duplex_mode  = cctx->duplex_mode;
    if (cctx->video_input > 0 && (unsigned int)cctx->video_input < FF_ARRAY_ELEMS(decklink_video_connection_map))
        ctx->video_input = decklink_video_connection_map[cctx->video_input];
    if (cctx->audio_input > 0 && (unsigned int)cctx->audio_input < FF_ARRAY_ELEMS(decklink_audio_connection_map))
        ctx->audio_input = decklink_audio_connection_map[cctx->audio_input];
    ctx->audio_pts_source = cctx->audio_pts_source;
    ctx->video_pts_source = cctx->video_pts_source;
    ctx->draw_bars = cctx->draw_bars;
    cctx->ctx = ctx;

    /* Check audio channel option for valid values: 2, 8 or 16 */
    switch (cctx->audio_channels) {
        case 2:
        case 8:
        case 16:
            break;
        default:
            av_log(avctx, AV_LOG_ERROR, "Value of channels option must be one of 2, 8 or 16\n");
            return AVERROR(EINVAL);
    }

    /* List available devices. */
    if (ctx->list_devices) {
        ff_decklink_list_devices_legacy(avctx, 1, 0);
        return AVERROR_EXIT;
    }

    if (cctx->v210) {
        av_log(avctx, AV_LOG_WARNING, "The bm_v210 option is deprecated and will be removed. Please use the -raw_format yuv422p10.\n");
        cctx->raw_format = MKBETAG('v','2','1','0');
    }

    strcpy (fname, avctx->filename);
    tmp=strchr (fname, '@');
    if (tmp != NULL) {
        av_log(avctx, AV_LOG_WARNING, "The @mode syntax is deprecated and will be removed. Please use the -format_code option.\n");
        mode_num = atoi (tmp+1);
        *tmp = 0;
    }

    ret = ff_decklink_init_device(avctx, fname);
    if (ret < 0)
        return ret;

    /* Get input device. */
    if (ctx->dl->QueryInterface(IID_IDeckLinkInput, (void **) &ctx->dli) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not open input device from '%s'\n",
               avctx->filename);
        ret = AVERROR(EIO);
        goto error;
    }

    /* List supported formats. */
    if (ctx->list_formats) {
        ff_decklink_list_formats(avctx, DIRECTION_IN);
        ret = AVERROR_EXIT;
        goto error;
    }

    if (mode_num > 0 || cctx->format_code) {
        if (ff_decklink_set_format(avctx, DIRECTION_IN, mode_num) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Could not set mode number %d or format code %s for %s\n",
                mode_num, (cctx->format_code) ? cctx->format_code : "(unset)", fname);
            ret = AVERROR(EIO);
            goto error;
        }
    }

#if !CONFIG_LIBZVBI
    if (ctx->teletext_lines && ctx->bmd_mode == bmdModePAL) {
        av_log(avctx, AV_LOG_ERROR, "Libzvbi support is needed for capturing SD PAL teletext, please recompile FFmpeg.\n");
        ret = AVERROR(ENOSYS);
        goto error;
    }
#endif

    /* Setup streams. */
    setup_audio(avctx);

    st = avformat_new_stream(avctx, NULL);
    if (!st) {
        av_log(avctx, AV_LOG_ERROR, "Cannot add stream\n");
        ret = AVERROR(ENOMEM);
        goto error;
    }
    st->codecpar->codec_type  = AVMEDIA_TYPE_VIDEO;
    st->codecpar->width       = ctx->bmd_width;
    st->codecpar->height      = ctx->bmd_height;

    st->time_base.den      = ctx->bmd_tb_den;
    st->time_base.num      = ctx->bmd_tb_num;
    av_stream_set_r_frame_rate(st, av_make_q(st->time_base.den, st->time_base.num));

    switch((BMDPixelFormat)cctx->raw_format) {
    case bmdFormat8BitYUV:
        st->codecpar->codec_id    = AV_CODEC_ID_RAWVIDEO;
        st->codecpar->codec_tag   = MKTAG('U', 'Y', 'V', 'Y');
        st->codecpar->format      = AV_PIX_FMT_UYVY422;
        st->codecpar->bit_rate    = av_rescale(ctx->bmd_width * ctx->bmd_height * 16, st->time_base.den, st->time_base.num);
        break;
    case bmdFormat10BitYUV:
        st->codecpar->codec_id    = AV_CODEC_ID_V210;
        st->codecpar->codec_tag   = MKTAG('V','2','1','0');
        st->codecpar->bit_rate    = av_rescale(ctx->bmd_width * ctx->bmd_height * 64, st->time_base.den, st->time_base.num * 3);
        st->codecpar->bits_per_coded_sample = 10;
        break;
    case bmdFormat8BitARGB:
        st->codecpar->codec_id    = AV_CODEC_ID_RAWVIDEO;
        st->codecpar->codec_tag   = avcodec_pix_fmt_to_codec_tag((enum AVPixelFormat)st->codecpar->format);;
        st->codecpar->format      = AV_PIX_FMT_ARGB;
        st->codecpar->bit_rate    = av_rescale(ctx->bmd_width * ctx->bmd_height * 32, st->time_base.den, st->time_base.num);
        break;
    case bmdFormat8BitBGRA:
        st->codecpar->codec_id    = AV_CODEC_ID_RAWVIDEO;
        st->codecpar->codec_tag   = avcodec_pix_fmt_to_codec_tag((enum AVPixelFormat)st->codecpar->format);
        st->codecpar->format      = AV_PIX_FMT_BGRA;
        st->codecpar->bit_rate    = av_rescale(ctx->bmd_width * ctx->bmd_height * 32, st->time_base.den, st->time_base.num);
        break;
    case bmdFormat10BitRGB:
        st->codecpar->codec_id    = AV_CODEC_ID_R210;
        st->codecpar->codec_tag   = MKTAG('R','2','1','0');
        st->codecpar->format      = AV_PIX_FMT_RGB48LE;
        st->codecpar->bit_rate    = av_rescale(ctx->bmd_width * ctx->bmd_height * 30, st->time_base.den, st->time_base.num);
        st->codecpar->bits_per_coded_sample = 10;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "Raw Format %.4s not supported\n", (char*) &cctx->raw_format);
        ret = AVERROR(EINVAL);
        goto error;
    }

    switch (ctx->bmd_field_dominance) {
    case bmdUpperFieldFirst:
        st->codecpar->field_order = AV_FIELD_TT;
        break;
    case bmdLowerFieldFirst:
        st->codecpar->field_order = AV_FIELD_BB;
        break;
    case bmdProgressiveFrame:
    case bmdProgressiveSegmentedFrame:
        st->codecpar->field_order = AV_FIELD_PROGRESSIVE;
        break;
    }

    avpriv_set_pts_info(st, 64, 1, 1000000);  /* 64 bits pts in us */

    ctx->video_st=st;

    if (ctx->teletext_lines) {
        st = avformat_new_stream(avctx, NULL);
        if (!st) {
            av_log(avctx, AV_LOG_ERROR, "Cannot add stream\n");
            ret = AVERROR(ENOMEM);
            goto error;
        }
        st->codecpar->codec_type  = AVMEDIA_TYPE_SUBTITLE;
        st->time_base.den         = ctx->bmd_tb_den;
        st->time_base.num         = ctx->bmd_tb_num;
        st->codecpar->codec_id    = AV_CODEC_ID_DVB_TELETEXT;
        avpriv_set_pts_info(st, 64, 1, 1000000);  /* 64 bits pts in us */
        ctx->teletext_st = st;
    }

    if (cctx->audio_mode == AUDIO_MODE_DISCRETE) {
        av_log(avctx, AV_LOG_VERBOSE, "Using %d input audio channels\n", ctx->audio_st[0]->codecpar->channels);
        result = ctx->dli->EnableAudioInput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, ctx->audio_st[0]->codecpar->channels);
    } else {
        /* FIXME: channel count hard-coded */
        av_log(avctx, AV_LOG_VERBOSE, "Using %d input audio channels\n", (int)ctx->max_audio_channels);
        result = ctx->dli->EnableAudioInput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger,
                                            ctx->max_audio_channels);
    }

    if (result != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Cannot enable audio input\n");
        ret = AVERROR(EIO);
        goto error;
    }

    result = ctx->dli->EnableVideoInput(ctx->bmd_mode,
                                        (BMDPixelFormat) cctx->raw_format,
                                        bmdVideoInputFlagDefault);

    if (result != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Cannot enable video input\n");
        ret = AVERROR(EIO);
        goto error;
    }

    avpacket_queue_init (avctx, &ctx->queue);

#if CONFIG_LIBKLVANC
    if (klvanc_context_create(&cctx->vanc_ctx) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create VANC library context\n");
    } else {
        cctx->vanc_ctx->verbose = 0;
        cctx->vanc_ctx->callbacks = &callbacks;
    }
#endif

    if (decklink_start_input (avctx) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Cannot start input stream\n");
        ret = AVERROR(EIO);
        goto error;
    }

    return 0;

error:
    ff_decklink_cleanup(avctx);
    return ret;
}

int ff_decklink_read_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;

    avpacket_queue_get(&ctx->queue, pkt, 1);

    return 0;
}

int ff_decklink_list_input_devices(AVFormatContext *avctx, struct AVDeviceInfoList *device_list)
{
    return ff_decklink_list_devices(avctx, device_list, 1, 0);
}

} /* extern "C" */
