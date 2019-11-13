/*
 * CEA-708 Closed Captioning FIFO
 * Copyright (c) 2019 LTN Global Communications
 *
 * Author: Devin Heitmueller <dheitmueller@ltnglobal.com>
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

#include "cc_fifo.h"

struct AVCCFifo {
    AVFifoBuffer *cc_608_fifo;
    AVFifoBuffer *cc_708_fifo;
    int expected_cc_count;
    int expected_608;
    int cc_detected;
    void *log_ctx;
};

#define MAX_CC_ELEMENTS 128
#define CC_BYTES_PER_ENTRY 3

struct cc_lookup {
    int num;
    int den;
    int cc_count;
    int num_608;
};

const static struct cc_lookup cc_lookup_vals[] = {
    { 15, 1, 40, 4 },
    { 30, 1, 20, 2 },
    { 30000, 1001, 20, 2},
    { 60, 1, 10, 1 },
    { 60000, 1001, 10, 1},
};

void av_cc_fifo_free(AVCCFifo *ccf)
{
    if (ccf) {
        if (ccf->cc_608_fifo)
            av_fifo_freep(&ccf->cc_608_fifo);
        if (ccf->cc_708_fifo)
            av_fifo_freep(&ccf->cc_708_fifo);
    }
}

AVCCFifo *av_cc_fifo_alloc(AVRational *framerate, void *log_ctx)
{
    AVCCFifo *ccf;
    int i;

    ccf = av_mallocz(sizeof(*ccf));
    if (!ccf)
        return NULL;

    if (!(ccf->cc_708_fifo = av_fifo_alloc_array(MAX_CC_ELEMENTS, CC_BYTES_PER_ENTRY)))
        goto error;

    if (!(ccf->cc_608_fifo = av_fifo_alloc_array(MAX_CC_ELEMENTS, CC_BYTES_PER_ENTRY)))
        goto error;


    /* Based on the target FPS, figure out the expected cc_count and number of
       608 tuples per packet.  See ANSI/CTA-708-E Sec 4.3.6.1. */
    for (i = 0; i < (sizeof(cc_lookup_vals) / sizeof(struct cc_lookup)); i++) {
        if (framerate->num == cc_lookup_vals[i].num &&
            framerate->den == cc_lookup_vals[i].den) {
            ccf->expected_cc_count = cc_lookup_vals[i].cc_count;
            ccf->expected_608 = cc_lookup_vals[i].num_608;
            break;
        }
    }

    if (ccf->expected_608 == 0) {
        av_log(ccf->log_ctx, AV_LOG_WARNING, "cc_fifo cannot transcode captions fps=%d/%d\n",
               framerate->num, framerate->den);
        return NULL;
    }

    return ccf;

error:
    av_cc_fifo_free(ccf);
    return NULL;
}

int av_cc_enqueue_avframe(AVCCFifo *ccf, AVFrame *frame)
{
    AVFrameSideData *sd;
    int cc_filled = 0;
    int i;

    if (ccf->cc_detected == 0 || ccf->expected_cc_count == 0)
        return 0;

    sd = av_frame_new_side_data(frame, AV_FRAME_DATA_A53_CC,
                                ccf->expected_cc_count * CC_BYTES_PER_ENTRY);
    if (!sd)
        return 0;

    for (i = 0; i < ccf->expected_608; i++) {
        if (av_fifo_size(ccf->cc_608_fifo) >= CC_BYTES_PER_ENTRY) {
            av_fifo_generic_read(ccf->cc_608_fifo, &sd->data[cc_filled * CC_BYTES_PER_ENTRY],
                                 CC_BYTES_PER_ENTRY, NULL);
            cc_filled++;
        } else {
            break;
        }
    }

    /* Insert any available data from the 708 FIFO */
    while (cc_filled < ccf->expected_cc_count) {
        if (av_fifo_size(ccf->cc_708_fifo) >= CC_BYTES_PER_ENTRY) {
            av_fifo_generic_read(ccf->cc_708_fifo, &sd->data[cc_filled * CC_BYTES_PER_ENTRY],
                                 CC_BYTES_PER_ENTRY, NULL);
            cc_filled++;
        } else {
            break;
        }
    }

    /* Insert 708 padding into any remaining fields */
    while (cc_filled < ccf->expected_cc_count) {
        sd->data[cc_filled * CC_BYTES_PER_ENTRY]     = 0xfa;
        sd->data[cc_filled * CC_BYTES_PER_ENTRY + 1] = 0x00;
        sd->data[cc_filled * CC_BYTES_PER_ENTRY + 2] = 0x00;
        cc_filled++;
    }
    return 0;
}

int av_cc_dequeue_avframe(AVCCFifo *ccf, AVFrame *frame)
{
    int i;

    /* Read the A53 side data, discard padding, and put 608/708 into
       queues so we can ensure they get into the output frames at
       the correct rate... */
    if (ccf->expected_cc_count > 0) {
        AVFrameSideData *side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_A53_CC);
        if (side_data) {
            uint8_t *cc_bytes = side_data->data;
            int cc_count = side_data->size / CC_BYTES_PER_ENTRY;
            ccf->cc_detected = 1;

            for (i = 0; i < cc_count; i++) {
                /* See ANSI/CTA-708-E Sec 4.3, Table 3 */
                uint8_t cc_valid = (cc_bytes[CC_BYTES_PER_ENTRY*i] & 0x04) >> 2;
                uint8_t cc_type = cc_bytes[CC_BYTES_PER_ENTRY*i] & 0x03;
                if (cc_type == 0x00 || cc_type == 0x01) {
                    av_fifo_generic_write(ccf->cc_608_fifo, &cc_bytes[CC_BYTES_PER_ENTRY*i],
                                          CC_BYTES_PER_ENTRY, NULL);
                } else if (cc_valid && (cc_type == 0x02 || cc_type == 0x03)) {
                    av_fifo_generic_write(ccf->cc_708_fifo, &cc_bytes[CC_BYTES_PER_ENTRY*i],
                                          CC_BYTES_PER_ENTRY, NULL);
                }
            }
            /* Remove the side data, as we will re-create it on the
               output as needed */
            av_frame_remove_side_data(frame, AV_FRAME_DATA_A53_CC);
        }
    }
    return 0;
}
