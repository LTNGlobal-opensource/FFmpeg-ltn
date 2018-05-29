/*
 * Copyright (c) 2018 LTN Global Communications Inc.
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
 * @ingroup lavu_vtune
 * Public header for generating logs usable by Intel VTune Amplifier
 */

#ifndef AVUTIL_VTUNE_H
#define AVUTIL_VTUNE_H

enum vtune_event_id {
    DECKLINK_BUFFER_COUNT,
    DECKLINK_BUFFERS_DROPPED,
    DECKLINK_BUFFERS_LATE,
    UDP_RX_FIFOSIZE,
    MAX_EVENT_ID
};

uint64_t av_vtune_get_timestamp(void);
void av_vtune_log_stat(enum vtune_event_id eventid, uint64_t value, int logthread);
void av_vtune_log_event(const char *event, uint64_t start_time, uint64_t end_time, int logthread);

#endif /* AVUTIL_VTUNE_H */
