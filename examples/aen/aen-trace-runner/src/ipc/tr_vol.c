/* src/ipc/tr_vol.c -- see tr_vol.h. */
#include "tr_vol.h"

bool tr_vol_valid(uint32_t word, uint32_t *pct)
{
	if ((word & TR_VOL_TAG_MASK) != TR_VOL_TAG || (word & ~TR_VOL_TAG_MASK) > TR_VOL_MAX) {
		return false;
	}
	*pct = word & ~TR_VOL_TAG_MASK;
	return true;
}

uint32_t tr_vol_read(uint32_t word)
{
	uint32_t pct;

	return tr_vol_valid(word, &pct) ? pct : TR_VOL_DEFAULT;
}

uint32_t tr_vol_gain(uint32_t pct)
{
	return (pct > TR_VOL_MAX ? TR_VOL_MAX : pct) * TR_VOL_UNITY / TR_VOL_MAX;
}

void tr_vol_ramp_init(tr_vol_ramp_t *r, uint32_t pct)
{
	r->gain = tr_vol_gain(pct);
}

void tr_vol_apply(tr_vol_ramp_t *r, int16_t *buf, unsigned n, uint32_t pct)
{
	const int64_t g0 = r->gain, g1 = tr_vol_gain(pct);

	r->gain = (uint32_t)g1;
	if (n == 0u || (g0 == TR_VOL_UNITY && g1 == TR_VOL_UNITY)) {
		return;
	}
	/* One division per block, not per sample (the HP has no 64-bit divide): the gain walks from
	 * g0 to g1 in Q32 steps; the truncated step falls short of g1 by under one Q16 LSB, which
	 * the S16 rounding below absorbs. */
	const int64_t step = ((g1 - g0) * 65536) / (int64_t)n;
	int64_t       acc  = g0 * 65536;

	for (unsigned i = 0; i < n; i++) {
		acc += step;
		int64_t g = acc >> 16;

		buf[i] = (int16_t)(((int64_t)buf[i] * g + (TR_VOL_UNITY / 2u)) >> 16);
	}
}

static void publish(tr_vol_he_t *he, volatile tr_vol_t *r)
{
	r->vol = tr_vol_word(he->pct);
}

static bool set(tr_vol_he_t *he, volatile tr_vol_t *r, uint32_t pct)
{
	if (pct == he->pct) {
		return false;
	}
	he->pct = pct;
	if (pct != 0u) {
		he->unmute_pct = pct;
	}
	publish(he, r);
	r->seq = r->seq + 1u;
	return true;
}

void tr_vol_he_boot(tr_vol_he_t *he, volatile tr_vol_t *r)
{
	he->pct        = TR_VOL_DEFAULT;
	he->unmute_pct = TR_VOL_DEFAULT;
	he->last_req   = r->req;
	r->rejects     = 0u;
	r->seq         = 0u;
	publish(he, r);
}

bool tr_vol_he_step(tr_vol_he_t *he, volatile tr_vol_t *r, int32_t detents, bool press)
{
	bool changed = false;

	if (detents != 0) {
		int32_t lim = (int32_t)(TR_VOL_MAX / TR_VOL_STEP);
		int32_t d   = detents > lim ? lim : (detents < -lim ? -lim : detents);
		int32_t p   = (int32_t)he->pct + d * (int32_t)TR_VOL_STEP;

		changed |=
		    set(he, r, (uint32_t)(p < 0 ? 0 : (p > (int32_t)TR_VOL_MAX ? (int32_t)TR_VOL_MAX : p)));
	}
	if (press) {
		changed |= set(he, r, he->pct != 0u ? 0u : he->unmute_pct);
	}
	uint32_t q = r->req;

	if (q != he->last_req) {
		uint32_t pct;

		he->last_req = q;
		if (tr_vol_valid(q, &pct)) {
			changed |= set(he, r, pct);
		} else {
			r->rejects = r->rejects + 1u;
		}
	}
	if (r->vol != tr_vol_word(he->pct)) {
		publish(he, r);
	}
	return changed;
}
