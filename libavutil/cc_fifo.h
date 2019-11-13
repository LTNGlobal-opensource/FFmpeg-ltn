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

/**
 * @file
 * CC FIFO Buffer
 */

#ifndef AVUTIL_CC_FIFO_H
#define AVUTIL_CC_FIFO_H

#include "avutil.h"
#include "frame.h"
#include "fifo.h"

typedef struct AVCCFifo AVCCFifo;

/**
 * Allocate an AVCCFifo.
 *
 * @param sample_fmt  sample format
 * @param channels    number of channels
 * @param nb_samples  initial allocation size, in samples
 * @return            newly allocated AVCCFifo, or NULL on error
 */
AVCCFifo *av_cc_fifo_alloc(AVRational *framerate, void *log_ctx);

/**
 * Free an AVCCFifo
 *
 * @param ccf AVCCFifo to free
 */
void av_cc_fifo_free(AVCCFifo *ccf);


/**
 * Read a frame into a CC Fifo
 *
 * Extract CC bytes from the AVFrame, insert them into our queue, and
 * remove the side data from the AVFrame.  The side data is removed
 * as it will be re-inserted at the appropriate rate later in the
 * filter.
 *
 * @param af          AVCCFifo to write to
 * @param frame       AVFrame with the video frame to operate on
 * @return            Zero on success, or negative AVERROR
 *                    code on failure.
 */
int av_cc_enqueue_avframe(AVCCFifo *af, AVFrame *frame);

/**
 * Insert CC side data into an AVFrame
 *
 * Dequeue the appropriate number of CC tuples based on the
 * frame rate, and insert them into the AVFrame
 *
 * @param af          AVCCFifo to read from
 * @param frame       AVFrame with the video frame to operate on
 * @return            Zero on success, or negative AVERROR
 *                    code on failure.
 */
int av_cc_dequeue_avframe(AVCCFifo *af, AVFrame *frame);

#endif /* AVUTIL_CC_FIFO_H */
