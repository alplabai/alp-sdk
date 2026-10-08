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

	/* The poll's deadline: called once a game frame (33.3 ms), the sample rate must be the period's,
	 * 10 Hz, not the whole frames above it (the old "now + period" gave 8.3 Hz on the bench). */
	{
		const int64_t period   = 100;
		int64_t       next_new = period, next_old = period;
		int           n_new = 0, n_old = 0;

		for (int f = 1; (int64_t)f * 3327 / 100 <= 315000;
		     f++) { /* 315 s of 30.06 Hz frames (33.27 ms), ms truncated */
			int64_t now = (int64_t)f * 3327 / 100;

			if (now >= next_new) {
				next_new = tr_pwr_next_deadline(next_new, now, period);
				n_new++;
			}
			if (now >= next_old) {
				next_old = now + period; /* the previous behaviour, as the control */
				n_old++;
			}
		}
		assert(n_new >= 3149 && n_new <= 3151); /* 10.00 Hz */
		assert(n_old <
		       2700); /* the control: a sample every 4 frames (7.5 Hz here; 8.3 on the bench) */
		/* a stall of 5 s: one sample, then back on the period -- no burst */
		assert(tr_pwr_next_deadline(1000, 6000, period) == 6100);
		assert(tr_pwr_next_deadline(1000, 1033, period) ==
		       1100); /* on time: exactly one period on */
		assert(tr_pwr_next_deadline(1000, 1100, period) == 1200); /* late to the ms */
	}

	printf("PASS: tests/host/test_pwr_ring.c\n");
	return 0;
}
