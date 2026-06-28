/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * fall_detect implementation -- see fall_detect.h.
 */
#include "fall_detect.h"

#include <math.h>

void fall_reset(struct fall_state *st)
{
	st->phase       = FALL_PHASE_NORMAL;
	st->ff_count    = 0;
	st->wait_count  = 0;
	st->post_count  = 0;
	st->still_count = 0;
	st->impact_g    = 0.0f;
}

bool fall_is_armed(const struct fall_state *st)
{
	return st->phase != FALL_PHASE_NORMAL;
}

bool fall_push(struct fall_state *st, float amag_g, float sr_hz, float *impact_g_out)
{
	/* Phase windows in samples, derived from the sample rate. */
	const uint16_t ff_min     = (uint16_t)(0.08f * sr_hz); /* >= ~80 ms free-fall */
	const uint16_t impact_win = (uint16_t)(0.30f * sr_hz); /* impact within ~300 ms */
	const uint16_t still_min  = (uint16_t)(1.00f * sr_hz); /* ~1 s stillness */
	const uint16_t post_max   = (uint16_t)(3.00f * sr_hz); /* give up after ~3 s */

	switch (st->phase) {
	case FALL_PHASE_NORMAL:
		if (amag_g < FALL_FREEFALL_G) {
			st->phase    = FALL_PHASE_FREEFALL;
			st->ff_count = 1;
		}
		break;

	case FALL_PHASE_FREEFALL:
		if (amag_g < FALL_FREEFALL_G) {
			st->ff_count++;
		} else if (st->ff_count >= ff_min) {
			/* Valid free-fall ended; look for the impact. */
			st->phase      = FALL_PHASE_WAIT_IMPACT;
			st->wait_count = 0;
			st->impact_g   = 0.0f;
			/* This same sample may already be the impact. */
			if (amag_g > FALL_IMPACT_G) {
				st->impact_g    = amag_g;
				st->phase       = FALL_PHASE_POST_IMPACT;
				st->post_count  = 0;
				st->still_count = 0;
			}
		} else {
			st->phase = FALL_PHASE_NORMAL; /* free-fall too short */
		}
		break;

	case FALL_PHASE_WAIT_IMPACT:
		st->wait_count++;
		if (amag_g > FALL_IMPACT_G) {
			st->impact_g    = amag_g;
			st->phase       = FALL_PHASE_POST_IMPACT;
			st->post_count  = 0;
			st->still_count = 0;
		} else if (st->wait_count > impact_win) {
			st->phase = FALL_PHASE_NORMAL; /* no impact after free-fall */
		}
		break;

	case FALL_PHASE_POST_IMPACT:
		st->post_count++;
		if (amag_g > st->impact_g) {
			st->impact_g = amag_g; /* track the spike peak. */
		}
		if (fabsf(amag_g - 1.0f) < 0.3f) {
			st->still_count++;
		} else {
			st->still_count = 0;
		}
		if (st->still_count >= still_min) {
			float impact = st->impact_g;
			fall_reset(st);
			if (impact_g_out != NULL) {
				*impact_g_out = impact;
			}
			return true; /* confirmed fall */
		}
		if (st->post_count > post_max) {
			fall_reset(st); /* never settled -> not a fall */
		}
		break;
	}
	return false;
}
