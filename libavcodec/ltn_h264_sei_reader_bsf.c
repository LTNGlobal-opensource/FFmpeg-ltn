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
    unsigned int last_hwrecd_tvsec;
    struct timeval hwrecd_current_second;
    double hwrecd_current_avg;
    double hwrecd_current_second_frames;
} ReaderContext;

static const unsigned char _uuid[] = 
{
    0x59, 0x96, 0xff, 0x28, 0x17, 0xca, 0x41, 0x96, 0x8d, 0xe3, 0xe5, 0x3f, 0xe2, 0xf9, 0x92, 0xae
};

static void timeval_add(struct timeval *result, struct timeval *x, struct timeval *y)
{
    result->tv_sec = x->tv_sec + y->tv_sec;

    unsigned int v = x->tv_usec + y->tv_usec;
    result->tv_sec += (v / 1000000);
    result->tv_usec = v % 1000000;
}

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
    struct timeval hwrecd, codecstart, codecend, codectime, totaltime, udpend;
    struct timeval encodertime;
    AVPacket *in;
    int ret, i = 0, j;
    uint32_t w, x, y, z;

    struct timeval now;
    gettimeofday(&now, NULL);

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
            for (j = i - 6; j < (i + sizeof(_uuid) + 30); j++)
                printf("%02x ", in->data[j]);
            printf("\n");
#endif

            /* Extract fields. */
            j = i + 16;

            /* Field 1: frame number */
            frameNumber  = in->data[j + 0] << 24;
            frameNumber |= in->data[j + 1] << 16;
            frameNumber |= in->data[j + 3] <<  8;
            frameNumber |= in->data[j + 4] <<  0;

            /* Field 2: Walltime when hardware received (secs) */
            hwrecd.tv_sec  = in->data[j +  6] << 24;
            hwrecd.tv_sec |= in->data[j +  7] << 16;
            hwrecd.tv_sec |= in->data[j +  9] <<  8;
            hwrecd.tv_sec |= in->data[j + 10] <<  0;

            /* Field 3: Walltime when hardware received (usecs) */
            hwrecd.tv_usec  = in->data[j + 12] << 24;
            hwrecd.tv_usec |= in->data[j + 13] << 16;
            hwrecd.tv_usec |= in->data[j + 15] <<  8;
            hwrecd.tv_usec |= in->data[j + 16] <<  0;
            x = hwrecd.tv_usec;

            /* Field 3: Walltime when going into video code (secs) */
            codecstart.tv_sec  = in->data[j + 18] << 24;
            codecstart.tv_sec |= in->data[j + 19] << 16;
            codecstart.tv_sec |= in->data[j + 21] <<  8;
            codecstart.tv_sec |= in->data[j + 22] <<  0;

            /* Field 4: Walltime when going into video code (usecs) */
            codecstart.tv_usec  = in->data[j + 24] << 24;
            codecstart.tv_usec |= in->data[j + 25] << 16;
            codecstart.tv_usec |= in->data[j + 27] <<  8;
            codecstart.tv_usec |= in->data[j + 28] <<  0;
            y = codecstart.tv_usec;

            /* Field 5: Walltime when frame leaving codec (secs) */
            codecend.tv_sec  = in->data[j + 30] << 24;
            codecend.tv_sec |= in->data[j + 31] << 16;
            codecend.tv_sec |= in->data[j + 33] <<  8;
            codecend.tv_sec |= in->data[j + 34] <<  0;

            /* Field 6: Walltime when frame leaving codec (usecs) */
            codecend.tv_usec  = in->data[j + 36] << 24;
            codecend.tv_usec |= in->data[j + 37] << 16;
            codecend.tv_usec |= in->data[j + 39] <<  8;
            codecend.tv_usec |= in->data[j + 40] <<  0;
            z = codecend.tv_usec;

            if (codecend.tv_sec && codecstart.tv_sec)
                timeval_subtract(&codectime, &codecend, &codecstart);
            else
                memset(&codectime, 0, sizeof(codectime));
            timeval_subtract(&totaltime, &now, &hwrecd);

            /* Field 7: Walltime when frame leaving udp transmitter (secs) */
            udpend.tv_sec  = in->data[j + 42] << 24;
            udpend.tv_sec |= in->data[j + 43] << 16;
            udpend.tv_sec |= in->data[j + 45] <<  8;
            udpend.tv_sec |= in->data[j + 46] <<  0;

            /* Field 8: Walltime when frame leaving udp transmitter (usecs) */
            udpend.tv_usec  = in->data[j + 48] << 24;
            udpend.tv_usec |= in->data[j + 49] << 16;
            udpend.tv_usec |= in->data[j + 51] <<  8;
            udpend.tv_usec |= in->data[j + 52] <<  0;
            w = udpend.tv_usec;

            timeval_subtract(&encodertime, &udpend, &hwrecd);

            s->hwrecd_current_second_frames++;
            struct timeval cs = s->hwrecd_current_second;
            timeval_add(&s->hwrecd_current_second, &totaltime, &cs);

            if (s->last_hwrecd_tvsec != hwrecd.tv_sec) {
                s->last_hwrecd_tvsec = hwrecd.tv_sec;
                s->hwrecd_current_avg = s->hwrecd_current_second.tv_sec * 1000000;
                s->hwrecd_current_avg += s->hwrecd_current_second.tv_usec;
                s->hwrecd_current_avg /= s->hwrecd_current_second_frames;
                s->hwrecd_current_avg /= 1000000;
                s->hwrecd_current_second.tv_sec = 0;
                s->hwrecd_current_second.tv_usec = 0;
                s->hwrecd_current_second_frames = 0;
            }

            if (s->rowcount++ == 0) {
                printf("frame        Encoder            Encoder            Codec              Codec             Codec Latency     Walltime minus    Walltime    Encoder Total\n");
                printf("Number       Entry Time------>  UDP Exit Time----> Entry Time------>  Exit Time-------> Time (Secs)---->  Encoder Entry-->  Average-->  Time (Secs)---->\n");
            }
            if (s->rowcount == 25)
                s->rowcount = 0;

#ifdef __LINUX__
            printf("%011d  %09ld.%06u  %09ld.%06lu  %09ld.%06lu  %09ld.%06lu  %09ld.%06lu  %09ld.%06lu  %010.03f  %09ld.%06lu\n",
                frameNumber,
                hwrecd.tv_sec, x,
                udpend.tv_sec, w,
                codecstart.tv_sec, y,
                codecend.tv_sec, z,
                codectime.tv_sec, codectime.tv_usec,
                totaltime.tv_sec, totaltime.tv_usec,
                s->hwrecd_current_avg,
                encodertime.tv_sec, encodertime.tv_usec);
#endif
#ifdef __APPLE__
            printf("%011d  %09lu.%06u  %09lu.%06u  %09lu.%06u  %09lu.%06u  %09lu.%06u  %09lu.%06u  %010.03f  %09lu.%06u\n",
                frameNumber,
                hwrecd.tv_sec, x,
                udpend.tv_sec, w,
                codecstart.tv_sec, y,
                codecend.tv_sec, z,
                codectime.tv_sec, codectime.tv_usec,
                totaltime.tv_sec, totaltime.tv_usec,
                s->hwrecd_current_avg,
                encodertime.tv_sec, encodertime.tv_usec);
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
