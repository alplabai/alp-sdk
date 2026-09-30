/* tests/host/test_flip.c -- free-buffer selection and overrun counting (src/ipc/tr_flip.c). */
#include <assert.h>
#include <stdio.h>

#include "../../src/ipc/tr_flip.h"

int main(void)
{
	/* The free buffer is always the other one; anything else is refused. */
	assert(tr_fb_free(TR_FB_A) == TR_FB_B);
	assert(tr_fb_free(TR_FB_B) == TR_FB_A);
	assert(tr_fb_free(0x02100000u) == 0u);
	assert(tr_fb_free(0u) == 0u);
	assert(tr_fb_valid(TR_FB_A) && tr_fb_valid(TR_FB_B));
	assert(!tr_fb_valid(0u) && !tr_fb_valid(TR_FB_A + 2u) && !tr_fb_valid(TR_MBOX_ADDR));

	tr_flip_pace_t p = { 0 };

	/* First flip has nothing to compare against. */
	assert(!tr_flip_pace_landed(&p, 1000000u));
	/* One refresh apart: fine. */
	assert(!tr_flip_pace_landed(&p, 1025000u));
	/* Exactly 1.5 refreshes (37.5 ms) is the boundary, not an overrun. */
	assert(!tr_flip_pace_landed(&p, 1025000u + 37500u));
	/* One microsecond past it is. */
	assert(tr_flip_pace_landed(&p, 1062500u + 37501u));
	/* Two refreshes (a slipped vblank, 20 Hz) is an overrun. */
	assert(tr_flip_pace_landed(&p, 1100001u + 50000u));
	/* The gap is measured from the previous flip, not accumulated. */
	assert(!tr_flip_pace_landed(&p, 1150001u + 25000u));

	/* A deliberate hold (reset) is not counted, however long. */
	tr_flip_pace_reset(&p);
	assert(!tr_flip_pace_landed(&p, 9000000u));
	assert(!tr_flip_pace_landed(&p, 9025000u));
	assert(tr_flip_pace_landed(&p, 9200000u));

	/* Flip-interval histogram: 25 ms steps, boundaries 27.5 / 52.5 / 77.5 ms. */
	assert(tr_flip_hist_bucket(0u) == 0u && tr_flip_hist_bucket(25000u) == 0u);
	assert(tr_flip_hist_bucket(27500u) == 0u && tr_flip_hist_bucket(27501u) == 1u);
	assert(tr_flip_hist_bucket(50000u) == 1u && tr_flip_hist_bucket(52500u) == 1u);
	assert(tr_flip_hist_bucket(52501u) == 2u && tr_flip_hist_bucket(77500u) == 2u);
	assert(tr_flip_hist_bucket(77501u) == 3u && tr_flip_hist_bucket(177500u) == 6u);
	assert(tr_flip_hist_bucket(177501u) == 7u && tr_flip_hist_bucket(60000000u) == 7u);
	{
		volatile uint32_t     h[TR_FLIP_HIST_N] = { 0 }, total = 0;
		tr_flip_pace_t        q    = { 0 };
		static const uint64_t at[] = { 1000000u, 1025000u, 1075000u, 1100000u, 1200000u };

		for (unsigned i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
			tr_flip_hist_note(&q, at[i], h, &total);
			(void)tr_flip_pace_landed(&q, at[i]);
		}
		/* first flip has no interval: 25, 50, 25, 100 ms */
		assert(total == 4u && h[0] == 2u && h[1] == 1u && h[3] == 1u);
		/* a hold (pace reset) is not an interval, however long */
		tr_flip_pace_reset(&q);
		tr_flip_hist_note(&q, 9000000u, h, &total);
		(void)tr_flip_pace_landed(&q, 9000000u);
		assert(total == 4u && h[7] == 0u);
		tr_flip_hist_note(&q, 9025000u, h, &total);
		assert(total == 5u && h[0] == 3u);
	}

	printf("test_flip: ok\n");
	return 0;
}
