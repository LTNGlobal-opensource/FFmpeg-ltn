/*
 * Copyright (c) 2019 LTN Global Communications Inc.
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
 * @ingroup lavu_ltn_log
 * Public API for reporting events back to LTN's controller
 */

#ifndef AVUTIL_LTNLOG_H
#define AVUTIL_LTNLOG_H

/* Various states of LTED can be in */
#define LTED_WAITING_FOR_INITIAL_DATA 0
#define LTED_PROBING                  1
#define LTED_SETUP_INPUT              2
#define LTED_SETUP_OUTPUT             3
#define LTED_RUNNING                  4

int ltnlog_setup(const char *url);
void ltnlog_stat(const char *str, uint64_t val);
void ltnlog_msg(const char *msgtype, const char *fmt, ...);

#endif /* AVUTIL_LTNLOG_H */
