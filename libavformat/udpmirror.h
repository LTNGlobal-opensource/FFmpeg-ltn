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

#ifndef AVUTIL_UDPMIRROR_H
#define AVUTIL_UDPMIRROR_H

int udpmirror_setup(const char *url);
void udpmirror_send(const void *buffer, size_t length);

#endif /* AVUTIL_URPMIRROR_H */
