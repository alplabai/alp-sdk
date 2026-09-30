/* src/game/tilt.c */
#include "tilt.h"

static int abs16(int16_t v)
{
	return (v < 0) ? -(int)v : (int)v;
}

static bool is_level(int16_t x_q8, int16_t y_q8)
{
	return abs16(x_q8) < TR_TILT_DEAD_Q8 && abs16(y_q8) < TR_TILT_DEAD_Q8;
}

void tr_tilt_init(tr_tilt_t *t)
{
	*t = (tr_tilt_t){ 0 };
}

tr_intent_t tr_tilt_intent(tr_tilt_t *t, int16_t x_q8, int16_t y_q8)
{
	tr_intent_t out = tr_intent_none();

	out.source = TR_INPUT_TILT;

	if (abs16(x_q8) < TR_TILT_DEAD_Q8) {
		t->steer_latched = false; /* back to level: the next tilt may fire again */
	} else if (!t->steer_latched && abs16(x_q8) >= TR_TILT_EDGE_Q8) {
		out.lane_delta =
		    (int8_t)(((x_q8 < 0) ? -1 : 1) * TR_TILT_STEER_SIGN); /* tilt.h: the mounting */
		t->steer_latched = true;
	}

	/*
	 * Pitch sign is a physical-orientation assumption (like track.c's
	 * TR_CAM_MIRROR_X/TR_CAM_FLIP_Y): top of the board tilted TOWARD the
	 * player (y more positive) = jump, AWAY (nose down) = duck. If it reads
	 * backwards on the bench, swap the two branches, not the sign of y.
	 *
	 * Known coupling on a VERTICALLY mounted panel: there one rest axis
	 * carries the full 1 g, and a steer roll of theta leaks 256*(1-cos theta)
	 * into y. At the steer edge (~10.8 deg) that is ~4.5 -- under DEAD; a
	 * ~36 deg roll would leak past EDGE 48. So a pitch only counts while it
	 * dominates the roll (|y| > |x|): a pure roll leaks 256*(1-cos) < 256*sin
	 * below 90 deg, so a hard sideways steer cannot fire a phantom JUMP.
	 */
	if (abs16(y_q8) < TR_TILT_DEAD_Q8) {
		t->pitch_latched = false;
	} else if (!t->pitch_latched && abs16(y_q8) >= TR_TILT_EDGE_Q8 && abs16(y_q8) > abs16(x_q8)) {
		out.jump         = (y_q8 > 0);
		out.duck         = (y_q8 < 0);
		t->pitch_latched = true;
	}

	if (out.lane_delta != 0 || out.jump || out.duck) {
		t->gestures++;
		t->idle_ticks = 0u;
	}
	return out;
}

tr_tilt_event_t tr_tilt_step(tr_tilt_t *t, int16_t x_q8, int16_t y_q8, tr_intent_t *out)
{
	*out = tr_intent_none();

	if (t->playing) {
		t->idle_ticks++;
		*out = tr_tilt_intent(t, x_q8, y_q8); /* zeroes idle_ticks on a gesture */
		if (t->idle_ticks >= TR_TILT_IDLE_TICKS) {
			t->playing = false;
			t->armed   = false; /* re-engage needs level first: see tilt.h */
			t->pinned  = false; /* the player has gone: the demo cycles again */
			t->walkaways++;
			*out = tr_intent_none();
			return TR_TILT_BACK_TO_ATTRACT;
		}
		return TR_TILT_STAY;
	}

	if (!TR_TILT_TAKEOVER) {
		return TR_TILT_STAY; /* default build: attract is pure self-play, see tilt.h */
	}

	/* Character pick (P16): a flick left / right past EDGE (rolling, not
	 * pitching) arms a pick in that direction (the steering's sign); it is
	 * made back inside the dead zone. A tilt held on into the takeover
	 * below drops it: the tilt that starts the run never also changes the
	 * character. One pick per flick, re-armed at level. */
	if (abs16(x_q8) < TR_TILT_DEAD_Q8) {
		if (t->pick_dir != 0) {
			t->character = (uint8_t)((t->character + TR_TILT_CHARS + t->pick_dir) % TR_TILT_CHARS);
			t->picks++;
			t->pinned = true;
		}
		t->pick_dir   = 0;
		t->pick_ready = true;
	} else if (t->pick_ready && abs16(x_q8) >= TR_TILT_EDGE_Q8 && abs16(x_q8) > abs16(y_q8)) {
		t->pick_dir   = (int8_t)(((x_q8 < 0) ? -1 : 1) * TR_TILT_STEER_SIGN);
		t->pick_ready = false;
	}

	/* Attract: count a held tilt only if it started from level, so a board
	 * left leaning past ENGAGE cannot cycle play -> walk-away -> play. */
	if (is_level(x_q8, y_q8)) {
		t->armed      = true;
		t->hold_ticks = 0u;
	} else if (t->armed && (abs16(x_q8) >= TR_TILT_ENGAGE_Q8 || abs16(y_q8) >= TR_TILT_ENGAGE_Q8)) {
		if (++t->hold_ticks >= TR_TILT_ENGAGE_TICKS) {
			t->playing       = true;
			t->armed         = false;
			t->hold_ticks    = 0u;
			t->idle_ticks    = 0u;
			t->steer_latched = true; /* the held engage tilt is not a gesture */
			t->pitch_latched = true;
			t->pick_dir      = 0; /* ... nor a pick */
			t->pick_ready    = false;
			t->engages++;
			return TR_TILT_START_RUN;
		}
	} else {
		t->hold_ticks = 0u; /* dropped back under ENGAGE: a lean or a knock, start over */
	}
	return TR_TILT_STAY;
}

bool tr_tilt_run_over(tr_tilt_t *t)
{
	if (t->idle_ticks < TR_TILT_OVER_IDLE_TICKS) {
		return false;
	}
	t->playing = false;
	t->armed   = false;
	t->pinned  = false;
	t->walkaways++;
	return true;
}

void tr_tilt_demo_over(tr_tilt_t *t)
{
	if (!t->pinned) {
		t->character = (uint8_t)((t->character + 1u) % TR_TILT_CHARS);
	}
}
