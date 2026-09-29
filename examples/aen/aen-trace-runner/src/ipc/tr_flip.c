/* src/ipc/tr_flip.c -- see tr_flip.h. */
#include "tr_flip.h"

bool tr_fb_valid(uint32_t fb)
{
	return fb == TR_FB_A || fb == TR_FB_B;
}

uint32_t tr_fb_free(uint32_t live)
{
	if (live == TR_FB_A)
		return TR_FB_B;
	if (live == TR_FB_B)
		return TR_FB_A;
	return 0u;
}

bool tr_flip_pace_landed(tr_flip_pace_t *p, uint64_t now_us)
{
	bool overran = p->have && (now_us - p->last_us) > TR_OVERRUN_GAP_US;

	p->last_us = now_us;
	p->have    = true;
	return overran;
}

void tr_flip_pace_reset(tr_flip_pace_t *p)
{
	p->have = false;
}

uint32_t tr_flip_hist_bucket(uint64_t gap_us)
{
	uint32_t k = 0;

	while (k < TR_FLIP_HIST_N - 1u && gap_us > TR_PANEL_PERIOD_US * k + TR_PANEL_PERIOD_US + 2500u)
		k++;
	return k;
}

void tr_flip_hist_note(const tr_flip_pace_t *p, uint64_t now_us, volatile uint32_t hist[TR_FLIP_HIST_N],
		       volatile uint32_t *total)
{
	if (!p->have)
		return;
	hist[tr_flip_hist_bucket(now_us - p->last_us)]++;
	(*total)++;
}
