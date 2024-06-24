
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#ifdef BUILDING_avutil
#include "sei-timestamp.h"
#else
#include <encoders/video/sei-timestamp.h>
#include "common/common.h"
#endif

int g_sei_timestamping = 0;

const unsigned char ltn_uuid_sei_timestamp[] =
{
    0x59, 0x96, 0xFF, 0x28, 0x17, 0xCA, 0x41, 0x96, 0x8D, 0xE3, 0xE5, 0x3F, 0xE2, 0xF9, 0x92, 0xAE
};

unsigned char *sei_timestamp_alloc(void)
{
	unsigned char *p = (unsigned char *)calloc(1, SEI_TIMESTAMP_PAYLOAD_LENGTH);
	if (!p)
		return NULL;

	memcpy(p, &ltn_uuid_sei_timestamp[0], sizeof(ltn_uuid_sei_timestamp));
	return p;
}

int sei_timestamp_init(unsigned char *buf, int lengthBytes)
{
	if (lengthBytes < (int)SEI_TIMESTAMP_PAYLOAD_LENGTH)
		return -1;

	memset(buf, 0, lengthBytes);
	memcpy(buf, &ltn_uuid_sei_timestamp[0], sizeof(ltn_uuid_sei_timestamp));

	return 0;
}

int sei_timestamp_field_set(unsigned char *buffer, int lengthBytes, uint32_t nr, uint32_t value)
{
	unsigned char *p = buffer;

	if (nr < 1 || nr > SEI_TIMESTAMP_FIELD_COUNT)
		return -1;

	p += (sizeof(ltn_uuid_sei_timestamp) + ((nr - 1) * 6));

	if (lengthBytes - (p - buffer) < 6) {
		return -1;
	}

	*(p++) = (value >> 24) & 0xff;
	*(p++) = (value >> 16) & 0xff;
	*(p++) = SEI_BIT_DELIMITER;
	*(p++) = (value >>  8) & 0xff;
	*(p++) = (value >>  0) & 0xff;
	*(p++) = SEI_BIT_DELIMITER;

	return 0;
}

int ltn_uuid_find(const unsigned char *buf, unsigned int lengthBytes)
{
	if (lengthBytes < SEI_TIMESTAMP_PAYLOAD_LENGTH)
		return -1;

	for (int i = 0; i <= (lengthBytes - (int)SEI_TIMESTAMP_PAYLOAD_LENGTH); i++) {
		if (memcmp(buf + i, &ltn_uuid_sei_timestamp[0], sizeof(ltn_uuid_sei_timestamp)) == 0) {
			return i;
		}
	}

	return -1;
}

int sei_timestamp_field_get(const unsigned char *buffer, int lengthBytes, uint32_t nr, uint32_t *value)
{
	uint32_t v;
	const unsigned char *p = buffer;

	if (nr < 1 || nr > SEI_TIMESTAMP_FIELD_COUNT)
		return -1;

	p += (sizeof(ltn_uuid_sei_timestamp) + ((nr - 1) * 6));
	v  = (*(p++) << 24);
	v |= (*(p++) << 16);
	p++;
	v |= (*(p++) <<  8);
	v |= (*(p++) <<  0);

	*value = v;

	return 0;
}

int64_t sei_timestamp_query_codec_latency_ms(const unsigned char *buffer, int lengthBytes)
{

	struct timeval begin, end;
	struct timeval diff;
	uint32_t v[8];
	for (int i = 0; i < 8; i++)
		sei_timestamp_field_get(buffer, lengthBytes, i, &v[i]);

	begin.tv_sec = v[4];
	begin.tv_usec = v[5];
	end.tv_sec = v[6];
	end.tv_usec = v[7];

	sei_timeval_subtract(&diff, &end, &begin);

#if 0
	printf("%08d: %d.%d - %d.%d = %d.%d\n",
		v[1], 
		begin.tv_sec, begin.tv_usec,
		end.tv_sec, end.tv_usec,
		diff.tv_sec, diff.tv_usec);
#endif

	return sei_timediff_to_msecs(&diff);
}

void sei_timestamp_hexdump(const unsigned char *buffer, int lengthBytes)
{
	int v = 0;
	int len = SEI_TIMESTAMP_PAYLOAD_LENGTH;
	if (lengthBytes < len)
		len = lengthBytes;

	for (int i = 1; i <= len; i++) {
		printf("%02x ", *(buffer + i - 1));
		if (i == sizeof(ltn_uuid_sei_timestamp))
			printf(" ");
		if (i > sizeof(ltn_uuid_sei_timestamp)) {
			if (v++ == 2) {
				printf(" ");
				v = 0;
			}
		}
	}
	printf("\n");
}

int sei_timestamp_value_timeval_query(const unsigned char *buffer, int lengthBytes, int nr, struct timeval *t)
{
	sei_timestamp_field_get(buffer, lengthBytes, nr, (uint32_t *)&t->tv_sec);
	sei_timestamp_field_get(buffer, lengthBytes, nr + 1, (uint32_t *)&t->tv_usec);
	return 0;
}

int sei_timestamp_value_timeval_set(const unsigned char *buffer, int lengthBytes, int nr, struct timeval *t)
{
	struct timeval ts;
	if (t) {
		ts = *t;
        } else {
		gettimeofday(&ts, NULL);
	}
	sei_timestamp_field_set((unsigned char *)buffer, lengthBytes, nr, (uint32_t)ts.tv_sec);
	sei_timestamp_field_set((unsigned char *)buffer, lengthBytes, nr + 1, (uint32_t)ts.tv_usec);
	return 0;
}

int sei_timeval_subtract(struct timeval *result, struct timeval *x /* now */, struct timeval *y /* then */)
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

int64_t sei_timediff_to_msecs(struct timeval *tv)
{
        return (tv->tv_sec * 1000) + (tv->tv_usec / 1000);
}

int64_t sei_timediff_to_usecs(struct timeval *tv)
{
        return (tv->tv_sec * 1000000) + tv->tv_usec;
}

