/* Copyright LiveTimeNet, Inc. 2017. All Rights Reserved. */

#include <stdio.h>
//#include <stdlib.h>
//#include <string.h>
//#include <unistd.h>
//#include <signal.h>
//#include <fcntl.h>
//#include <curses.h>
#include <inttypes.h>
#include <pthread.h>
#include <errno.h>
#include "udpstats.h"

void udp_stats(struct tool_context_s *ctx, unsigned char *buf, int byteCount)
{
    time_t now;
    if (ctx->ofh) {
        size_t wlen = fwrite(buf, 1, byteCount, ctx->ofh);

        if (wlen != byteCount) {
            fprintf(stderr, "Warning: unable to write output\n");
        }
    }

    time(&now);

    if (now != ctx->bytesWrittenTime) {
        ctx->bytesWrittenTime = now;
        ctx->bytesWritten = ctx->bytesWrittenCurrent;
        ctx->bytesWrittenCurrent = 0;
    }

    if (buf[0] == 0x47) {
        /* MPEG-TS in UDP */
    } else if (buf[0] != 0x47 && buf[12] == 0x47) {
        /* MPEG-TS in RTP.  Skip over RTP header */
        buf += 12;
        byteCount -= 12;
    } else {
        /* No idea what this is, so don't try to pluck out MPEG-TS
           properties */
        return;
    }

    for (int i = 0; i < byteCount; i += 188) {
        uint16_t pidnr = getPID(buf + i);
        struct pid_statistics_s *pid = &ctx->stream.pids[pidnr];
        uint8_t cc;

        ctx->bytesWrittenCurrent += 188;

        pid->enabled = 1;
        pid->packetCount++;

        cc = getCC(buf + i);
        if (isCCInError(buf + i, pid->lastCC)) {
            if (pid->packetCount > 1 && pidnr != 0x1fff) {
                pid->ccErrors++;
            }
        }

        pid->lastCC = cc;

        if (isTEI(buf + i))
            pid->teiErrors++;

        if (ctx->verbose) {
            for (int i = 0; i < byteCount; i += 188) {
                for (int j = 0; j < 24; j++) {
                    printf("%02x ", buf[i + j]);
                    if (j == 3)
                        printf("-- 0x%04x(%4d) -- ", pidnr, pidnr);
                }
                printf("\n");
            }
        }
    }

    return;
}

int isCCInError(uint8_t *pkt, uint8_t oldCC)
{
    unsigned int adap = getPacketAdaption(pkt);
    unsigned int cc = getCC(pkt);

    if (((adap == 0) || (adap == 2)) && (oldCC == cc))
        return 0;

    if (((adap == 1) || (adap == 3)) && (oldCC == cc))
        return 1;

    if (((oldCC + 1) & 0x0f) == cc)
        return 0;

    return 1;
}

