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

/* Utility functions for reporting back various stats to LTN's controller
   infrastructure */

#include "config.h"

#define _GNU_SOURCE
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "libavformat/avformat.h"
#include "libavformat/network.h"
#include "libavutil/avstring.h"
#include "ltnlog.h"

/* Global providing the socket to log to */
int ltn_log_fd = -1;

int ltnlog_setup(const char *url)
{
    char hostname[256];
    int port;
    int error;
    char sport[16];
    struct addrinfo hints = { 0 }, *res = 0;

    av_url_split(NULL, 0, NULL, 0, hostname, sizeof(hostname), &port, NULL,
                 0, url);

    /* This is all cribbed from libavformat/udp.c */
    snprintf(sport, sizeof(sport), "%d", port);
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family   = AF_UNSPEC;

    if ((error = getaddrinfo(hostname, sport, &hints, &res))) {
        res = NULL;
#if 0
        av_log(avctx, AV_LOG_ERROR, "getaddrinfo(%s, %s): %s\n",
               hostname, sport, gai_strerror(error));
#endif
    } else {
        struct sockaddr_storage dest_addr;
        int dest_addr_len;

        memcpy(&dest_addr, res->ai_addr, res->ai_addrlen);
        dest_addr_len = res->ai_addrlen;

        ltn_log_fd = ff_socket(res->ai_family, SOCK_DGRAM, 0, NULL);
        if (ltn_log_fd < 0) {
#if 0
            av_log(avctx, AV_LOG_ERROR, "Call to ff_socket failed\n");
#endif
        } else {
            if (connect(ltn_log_fd, (struct sockaddr *) &dest_addr,
                        dest_addr_len)) {
#if 0
                av_log(avctx, AV_LOG_ERROR, "Failure to connect to monitor port\n");
#endif
                closesocket(ltn_log_fd);
                ltn_log_fd = -1;
            }
        }
    }
    return 0;
}

void ltnlog_stat(const char *str, uint64_t val)
{
    char *buf;

    if (ltn_log_fd < 0)
        return;

    buf = av_asprintf("%s: %" PRId64 "\n", str, val);
    if (!buf)
        return;

    send(ltn_log_fd, buf, strlen(buf), 0);
    av_free(buf);
}

void ltnlog_msg(const char *msgtype, const char *fmt, ...)
{
    va_list va;
    char buf[1024];
    char final[1024];

    if (ltn_log_fd < 0)
        return;

    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);

    final[0] = 0x00;
    av_strlcatf(final, sizeof(final), "%s: %s", msgtype, buf);

    send(ltn_log_fd, final, strlen(final), 0);
}
