/* tests/host/test_pwr_ring.c -- src/platform/pwr_ring.h: the +5V net's power-graph ring. A missed
 * sample is a gap, the window reads oldest first, and nothing before the first push is data. */
#include <assert.h>
#include <stdio.h>

#include "../../src/platform/pwr_ring.h"

int main(void)
{
	tr_pwr_ring_t r;
	int16_t       w[TR_PWR_N];

	tr_pwr_ring_init(&r);
	assert(tr_pwr_ring_read(&r, w) == 0u);
	for (int i = 0; i < TR_PWR_N; i++) {
		assert(w[i] == TR_PWR_GAP); /* nothing pushed: no data, not a flat zero */
	}

	tr_pwr_ring_push(&r, 2100);
	tr_pwr_ring_push(&r, -1); /* a miss */
	tr_pwr_ring_push(&r, 0);  /* a real zero is a sample */
	assert(tr_pwr_ring_read(&r, w) == 3u);
	assert(w[TR_PWR_N - 3] == 2100 && w[TR_PWR_N - 2] == TR_PWR_GAP && w[TR_PWR_N - 1] == 0);
	assert(w[TR_PWR_N - 4] == TR_PWR_GAP); /* older than the first push */

	/* more than a window: the oldest fall off, the order stays oldest first */
	for (int n = 0; n < 200; n++) {
		tr_pwr_ring_push(&r, 1000 + n);
	}
	assert(tr_pwr_ring_read(&r, w) == 203u);
	for (int i = 0; i < TR_PWR_N; i++) {
		assert(w[i] == 1000 + 200 - TR_PWR_N + i);
	}

	/* exactly one window, and wrap-around of the write index */
	tr_pwr_ring_init(&r);
	for (int n = 0; n < TR_PWR_N; n++) {
		tr_pwr_ring_push(&r, 10 + n);
	}
	assert(tr_pwr_ring_read(&r, w) == (uint32_t)TR_PWR_N);
	assert(w[0] == 10 && w[TR_PWR_N - 1] == 10 + TR_PWR_N - 1);
	tr_pwr_ring_push(&r, 7);
	(void)tr_pwr_ring_read(&r, w);
	assert(w[0] == 11 && w[TR_PWR_N - 1] == 7);

	/* an int16 cannot hold it: clamped, never wrapped into a negative that reads as a gap */
	tr_pwr_ring_push(&r, 70000);
	(void)tr_pwr_ring_read(&r, w);
	assert(w[TR_PWR_N - 1] == INT16_MAX);

	printf("PASS: tests/host/test_pwr_ring.c\n");
	return 0;
}
