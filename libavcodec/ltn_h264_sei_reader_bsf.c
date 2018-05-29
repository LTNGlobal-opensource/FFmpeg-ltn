/*
 * This file is part of FFmpeg.
 *
 * Based on NULL BSF.
 * Copyright LTN. 2018 <stoth@ltnglobal.com>
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
 * @file
 * LTN H264 SEI Monitor bitstream filter -- Extract and calculate codec
 * frame times from date/time stamps in the SEI.
 * Pass the input through unchanged.
 *
 * Test with:
 * ./ffmpeg -i /tmp/extra-sei8.ts -c:v copy -bsf:v ltn_h264_sei_reader -f null -
 */

#include "avcodec.h"
#include "bsf.h"

#include <sys/time.h>

typedef struct ReaderContext
{
    int rowcount;
} ReaderContext;

static const unsigned char _uuid[] = 
{
    0x59, 0x96, 0xff, 0x28, 0x17, 0xca, 0x41, 0x96, 0x8d, 0xe3, 0xe5, 0x3f, 0xe2, 0xf9, 0x92, 0xae
};

static int timeval_subtract(struct timeval *result, struct timeval *x, struct timeval *y)
{
    /* Perform the carry for the later subtraction by updating y. */
    if (x->tv_usec < y->tv_usec)
    {
        int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
        y->tv_usec -= 1000000 * nsec;
        y->tv_sec += nsec;
    }
    if (x->tv_usec - y->tv_usec > 1000000)
    {
        int nsec = (x->tv_usec - y->tv_usec) / 1000000;
        y->tv_usec += 1000000 * nsec;
        y->tv_sec -= nsec;
    }

    /* Compute the time remaining to wait. tv_usec is certainly positive. */
    result->tv_sec = x->tv_sec - y->tv_sec;
    result->tv_usec = x->tv_usec - y->tv_usec;

    /* Return 1 if result is negative. */
    return x->tv_sec < y->tv_sec;
}

static int ltn_h264_sei_reader_filter(AVBSFContext *ctx, AVPacket *out)
{
    ReaderContext *s = ctx->priv_data;
    uint32_t frameNumber;
    struct timeval hw_received;
    struct timeval codecBegin, codecEnd, codecDiff;
    struct timeval hardwareToCodecDiff;
    AVPacket *in;
    int ret, i = 0, j;
    uint32_t w, x;

    ret = ff_bsf_get_packet(ctx, &in);
    if (ret < 0)
        return ret;

    /* We're handed one or more NALs. Rather than build a NAL parser, here's
     * a quick search/locate implementation that looks for the 16byte uuid
     * present in the NAL we're trying to locate. When we find it, extract
     * the fields and computer time differences.
     */

    while (i < (in->size - sizeof(_uuid))) {
        if (memcmp(&in->data[i], _uuid, sizeof(_uuid)) == 0) {
#if 0
            for (j = i - 6; j < (i + sizeof(_uuid) + 41); j++)
                printf("%02x ", in->data[j]);
            printf("\n");
#endif

            /* Extract fields. */
            j = i + 16;
            frameNumber  = in->data[j + 0] << 24;
            frameNumber |= in->data[j + 1] << 16;
            // bit delimiter
            frameNumber |= in->data[j + 3] <<  8;
            frameNumber |= in->data[j + 4] <<  0;
            // bit delimiter

            hw_received.tv_sec  = in->data[j +  6] << 24;
            hw_received.tv_sec |= in->data[j +  7] << 16;
            // bit delimiter
            hw_received.tv_sec |= in->data[j +  9] <<  8;
            hw_received.tv_sec |= in->data[j + 10] <<  0;
            // bit delimiter

            hw_received.tv_usec  = in->data[j + 12] << 24;
            hw_received.tv_usec |= in->data[j + 13] << 16;
            // bit delimiter
            hw_received.tv_usec |= in->data[j + 15] <<  8;
            hw_received.tv_usec |= in->data[j + 16] <<  0;
            // bit delimiter
            w = hw_received.tv_usec;

            codecBegin.tv_sec  = in->data[j + 18] << 24;
            codecBegin.tv_sec |= in->data[j + 19] << 16;
            // bit delimiter
            codecBegin.tv_sec |= in->data[j + 21] <<  8;
            codecBegin.tv_sec |= in->data[j + 22] <<  0;
            // bit delimiter

            codecBegin.tv_usec  = in->data[j + 24] << 24;
            codecBegin.tv_usec |= in->data[j + 25] << 16;
            // bit delimiter
            codecBegin.tv_usec |= in->data[j + 27] <<  8;
            codecBegin.tv_usec |= in->data[j + 28] <<  0;
            // bit delimiter
            x = codecBegin.tv_usec;

            codecEnd.tv_sec  = in->data[j + 30] << 24;
            codecEnd.tv_sec |= in->data[j + 31] << 16;
            // bit delimiter
            codecEnd.tv_sec |= in->data[j + 33] <<  8;
            codecEnd.tv_sec |= in->data[j + 34] <<  0;
            // bit delimiter

            codecEnd.tv_usec  = in->data[j + 36] << 24;
            codecEnd.tv_usec |= in->data[j + 37] << 16;
            // bit delimiter
            codecEnd.tv_usec |= in->data[j + 39] <<  8;
            codecEnd.tv_usec |= in->data[j + 40] <<  0;
            // bit delimiter

            timeval_subtract(&codecDiff, &codecEnd, &codecBegin);
            timeval_subtract(&hardwareToCodecDiff, &codecEnd, &hw_received);

            if (s->rowcount++ == 0) {
                printf("frame        Hardware           Codec              Codec              Codec Latency       HW 2 Codec Latency\n");
                printf("Number       Capture Time---->  Entry Time------>  Exit Time------->  Time (Seconds)--->  Time (Seconds)--->\n");
            }
            if (s->rowcount == 25)
                s->rowcount = 0;

#ifdef __LINUX__
            printf("%011d  %09ld.%06u  %09ld.%06u  %09ld.%06lu  %11ld.%06lu  %11ld.%06lu\n",
                frameNumber,
                hw_received.tv_sec, w,
                codecBegin.tv_sec, x,
                codecEnd.tv_sec, codecEnd.tv_usec,
                codecDiff.tv_sec, codecDiff.tv_usec,
                hardwareToCodecDiff.tv_sec, hardwareToCodecDiff.tv_usec);
#endif
#ifdef __APPLE__
            printf("%011d  %09lu.%06u  %09lu.%06u  %09lu.%06u  %11lu.%06u  %11lu.%06u\n",
                frameNumber,
                hw_received.tv_sec, w,
                codecBegin.tv_sec, x,
                codecEnd.tv_sec, codecEnd.tv_usec,
                codecDiff.tv_sec, codecDiff.tv_usec,
                hardwareToCodecDiff.tv_sec, hardwareToCodecDiff.tv_usec);
#endif

            break;
	}
        i++;
    }

    av_packet_move_ref(out, in);
    av_packet_free(&in);
    return 0;
}

const AVBitStreamFilter ff_ltn_h264_sei_reader_bsf = {
    .name           = "ltn_h264_sei_reader",
    .priv_data_size = sizeof(ReaderContext),
    .filter         = ltn_h264_sei_reader_filter,
};
