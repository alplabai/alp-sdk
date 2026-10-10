/* src/ipc/tr_knob.c -- see tr_knob.h. */
#include "tr_knob.h"

bool tr_bl_valid(uint32_t word, uint32_t *pct)
{
	uint32_t p = word & ~TR_VOL_TAG_MASK;

	if ((word & TR_VOL_TAG_MASK) != TR_BL_TAG || p < TR_BL_MIN || p > TR_BL_MAX ||
	    p % TR_BL_STEP != 0u) {
		return false;
	}
	*pct = p;
	return true;
}

static bool set_bl(tr_knob_t *k, volatile tr_bl_t *r, uint32_t pct)
{
	if (pct == k->bl_pct) {
		return false;
	}
	k->bl_pct = pct;
	r->bl     = tr_bl_word(pct);
	r->seq    = r->seq + 1u;
	k->seq++;
	return true;
}

void tr_knob_boot(tr_knob_t *k, volatile tr_bl_t *r, uint32_t bl_pct, bool has_bl)
{
	*k = (tr_knob_t){ 0 };
	if (bl_pct < TR_BL_MIN) {
		bl_pct = TR_BL_MIN;
	} else if (bl_pct > TR_BL_MAX) {
		bl_pct = TR_BL_MAX;
	}
	k->mode     = TR_KNOB_VOLUME;
	k->bl_pct   = bl_pct;
	k->has_bl   = has_bl;
	r->rejects  = 0u;
	r->led_errs = 0u;
	r->req      = 0u;
	r->seq      = 0u;
	r->bl       = tr_bl_word(bl_pct);
}

tr_knob_out_t
tr_knob_step(tr_knob_t *k, volatile tr_bl_t *r, uint32_t now_ms, int32_t detents, bool sw)
{
	tr_knob_out_t out = { 0 };

	out.bl_prev = k->bl_pct;

	/* debounce: a new raw state counts once it has held TR_KNOB_DEBOUNCE_MS */
	if (sw != k->raw) {
		k->raw    = sw;
		k->raw_ms = now_ms;
	}
	if (k->raw != k->down && now_ms - k->raw_ms >= TR_KNOB_DEBOUNCE_MS) {
		k->down        = k->raw;
		k->last_act_ms = now_ms;
		if (k->down) {
			k->down_ms    = k->raw_ms;
			k->long_fired = false;
		} else if (!k->long_fired && k->has_bl) { /* a short press */
			k->mode = k->mode == TR_KNOB_VOLUME ? TR_KNOB_BRIGHTNESS : TR_KNOB_VOLUME;
			k->seq++;
		}
	}
	/* The long press is measured to the RAW edge: the switch still held (raw) and TR_KNOB_LONG_MS
	 * since it went down. A release at 970..999 ms is inside the debounce window, k->down is not
	 * cleared yet, and it must stay a short press. */
	if (k->down) {
		k->last_act_ms = now_ms; /* a held switch is activity: no revert under the finger */
		if (k->raw && !k->long_fired && now_ms - k->down_ms >= TR_KNOB_LONG_MS) {
			k->long_fired = true;
			out.mute      = true;
			k->mode       = TR_KNOB_VOLUME; /* the mute popup names the volume */
			k->seq++;
		}
	}

	if (detents != 0) {
		k->last_act_ms = now_ms;
		if (k->mode == TR_KNOB_VOLUME) {
			out.vol_detents = detents;
		} else {
			int32_t lim = (int32_t)(TR_BL_MAX / TR_BL_STEP);
			int32_t d   = detents > lim ? lim : (detents < -lim ? -lim : detents);
			int32_t p   = (int32_t)k->bl_pct + d * (int32_t)TR_BL_STEP;

			p = p < (int32_t)TR_BL_MIN ? (int32_t)TR_BL_MIN
			                           : (p > (int32_t)TR_BL_MAX ? (int32_t)TR_BL_MAX : p);
			out.bl_changed |= set_bl(k, r, (uint32_t)p);
		}
	}

	uint32_t q = r->req;

	if (q != 0u) { /* a request: taken once, accepted or refused, and the word handed back */
		uint32_t pct;

		r->req = 0u;
		if (k->has_bl && tr_bl_valid(q, &pct)) {
			out.bl_changed |= set_bl(k, r, pct);
			k->mode        = TR_KNOB_BRIGHTNESS; /* the popup names what was changed */
			k->last_act_ms = now_ms;
			k->seq++;
		} else {
			r->rejects = r->rejects + 1u; /* bad word, or no backlight to set */
		}
	}

	if (k->mode == TR_KNOB_BRIGHTNESS && !k->down && now_ms - k->last_act_ms >= TR_KNOB_IDLE_MS) {
		k->mode = TR_KNOB_VOLUME; /* no seq: the revert is silent, no popup */
	}
	if (r->bl != tr_bl_word(k->bl_pct)) {
		r->bl = tr_bl_word(k->bl_pct); /* the bench moves the level with req, not by writing bl */
	}
	return out;
}

void tr_knob_bl_failed(tr_knob_t *k, volatile tr_bl_t *r, uint32_t prev_pct)
{
	k->bl_pct   = prev_pct;
	r->bl       = tr_bl_word(prev_pct);
	r->led_errs = r->led_errs + 1u;
}
