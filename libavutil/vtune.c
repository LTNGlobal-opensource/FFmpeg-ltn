/*
 * Copyright (c) 2014 LTN Global Communications Inc.
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

#include "config.h"

#define _GNU_SOURCE
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "thread.h"
#include "common.h"
#include "vtune.h"

static FILE *log_fds[MAX_EVENT_ID];
static FILE *event_fd;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *event_names[] = {
    "DecklinkBuffersFree.INST",
    "DecklinkBuffersDropped.INST",
    "DecklinkBuffersLate.INST",
    "UDPFifoRxSize.INST",
};

static uint64_t av_vtune_get_threadid(void)
{
#if defined(__APPLE__)
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    return tid;
#else
    return syscall(SYS_gettid);
#endif

}

uint64_t av_vtune_get_timestamp(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (t.tv_sec * 1000000000) + t.tv_nsec;
}

void av_vtune_log_stat(enum vtune_event_id eventid, uint64_t value, int logthread)
{
    if (eventid >= MAX_EVENT_ID)
        return;

    if (log_fds[eventid] == NULL) {
        char filename[128];
        const char *vtune_hostname = getenv("AMPLXE_HOSTNAME");
        if (vtune_hostname == NULL)
            return;
        snprintf(filename, sizeof(filename), "/tmp/vtune%d-hostname-%s.csv", eventid,
                 vtune_hostname);
        log_fds[eventid] = fopen(filename, "w+");
        if (log_fds[eventid] != NULL) {
            fprintf(log_fds[eventid], "tsc.CLOCK_MONOTONIC_RAW,%s,pid,tid\n",
                event_names[eventid]);
        }
    }


    if (log_fds[eventid] == NULL)
        return;

    pthread_mutex_lock(&mutex);
    if (logthread) {
        fprintf(log_fds[eventid], "%"PRIu64",%"PRIu64",%d,%"PRIu64"\n",
                av_vtune_get_timestamp(), value, getpid(), av_vtune_get_threadid());
    } else {
        fprintf(log_fds[eventid], "%"PRIu64",%"PRIu64",%d,\n",
                av_vtune_get_timestamp(), value, getpid());
    }
    pthread_mutex_unlock(&mutex);

    fflush(log_fds[eventid]);
}

void av_vtune_log_event(const char *event, uint64_t start_time, uint64_t end_time, int logthread)
{
    if (event_fd == NULL) {
        char filename[128];
        const char *vtune_hostname = getenv("AMPLXE_HOSTNAME");
        if (vtune_hostname == NULL)
            return;

        snprintf(filename, sizeof(filename), "/tmp/vtune-hostname-%s.csv",
                 vtune_hostname);
        event_fd = fopen(filename, "w+");
        if (event_fd != NULL) {
            fprintf(event_fd, "name,start_tsc.CLOCK_MONOTONIC_RAW,end_tsc,pid,tid\n");
        }
    }

    if (event_fd == NULL)
        return;

    if (logthread) {
        fprintf(event_fd, "%s,%"PRIu64",%"PRIu64",%d,%"PRIu64"\n", event, start_time, end_time,
                getpid(), av_vtune_get_threadid());
    } else {
        fprintf(event_fd, "%s,%"PRIu64",%"PRIu64",%d,\n", event, start_time, end_time, getpid());
    }
    fflush(event_fd);
}
