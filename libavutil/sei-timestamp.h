#ifndef SEI_TIMESTAMP_H
#define SEI_TIMESTAMP_H

#include <stdint.h>
#include <sys/time.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* Marker to prevent 21 consequtive zeros, its illegal. */
#define SEI_BIT_DELIMITER 0x81

extern const unsigned char ltn_uuid_sei_timestamp[16];
extern int g_sei_timestamping;

/* Format of LTN_SEI_TAG_START_TIME record:
 * All records are big endian.
 * Field#              : Description
 *         TT TT TT TT : 16 byte UUID (ltn_uuid_sei_timestamp)
 * 1       FC FC FC FC : incrementing frame counter.
 * 2       HS HS HS HS : time received from hardware seconds (timeval.ts_sec).
 * 3       HU HU HU HU : time received from hardware useconds (timeval.ts_usec).
 * 4       SS SS SS SS : time send to compressor seconds (timeval.ts_sec).
 * 5       SU SU SU SU : time send to compressor useconds (timeval.ts_usec).
 * 6       ES ES ES ES : time exit from compressor seconds (timeval.ts_sec).
 * 7       EU EU EU EU : time exit from compressor useconds (timeval.ts_usec).
 * 8       EN EN EN EN : time exit from udp transmitter (timeval.ts_sec).
 * 9       EN EN EN EN : time exit from udp transmitter (timeval.ts_usec).
 */

#define SEI_TIMESTAMP_FIELD_COUNT (9)
#define SEI_TIMESTAMP_PAYLOAD_LENGTH (sizeof(ltn_uuid_sei_timestamp) + (SEI_TIMESTAMP_FIELD_COUNT * 6))

unsigned char *sei_timestamp_alloc(void);
int            sei_timestamp_init(unsigned char *buffer, int lengthBytes);
int            sei_timestamp_field_set(unsigned char *buffer, int lengthBytes, uint32_t nr, uint32_t value);
int            sei_timestamp_field_get(const unsigned char *buffer, int lengthBytes, uint32_t nr, uint32_t *value);

/* Find UUID in buffer, return buffer index or < 0 if found found. */
int ltn_uuid_find(const unsigned char *buf, unsigned int lengthBytes);

int64_t sei_timestamp_query_codec_latency_ms(const unsigned char *buffer, int lengthBytes);
void sei_timestamp_hexdump(const unsigned char *buffer, int lengthBytes);

int sei_timestamp_value_timeval_query(const unsigned char *buffer, int lengthBytes, int nr, struct timeval *t);

/* Value 2, time of hardware when frame arrived.
 * Value 4, time of frame codec entry point.
 * Value 6, time of frame codec exit point.
 */
int sei_timestamp_value_timeval_set(const unsigned char *buffer, int lengthBytes, int nr, struct timeval *t);

int sei_timeval_subtract(struct timeval *result, struct timeval *x, struct timeval *y);
int64_t sei_timediff_to_msecs(struct timeval *tv);
int64_t sei_timediff_to_usecs(struct timeval *tv);

#if defined(__cplusplus)
};
#endif
#endif /* SEI_TIMESTAMP_H */
