/* src/vision/track.c */
#include "track.h"

/*
 * Body tracking, not gesture classification of a whole posture: the arm
 * controls are two numbers per pose (arms.c), the duck falls out of one torso
 * box.
 *
 * Everything here is pure: no camera, no SDK, no allocation.  That is what lets
 * the whole control scheme be tested on the host before touching hardware.
 */

static void reset_duck(tr_track_t *t)
{
	t->win_n = t->win_i = 0u;
	t->duck_pend = t->duck_on = 0u;
}

/* Append one measured pose's shoulder width. */
static void win_push(tr_track_t *t, int16_t w)
{
	t->win_w[t->win_i] = w;
	t->win_i           = (uint8_t)((t->win_i + 1u) % 3u);
	if (t->win_n < 3u) {
		t->win_n++;
	}
}

/* A new person, the same one much closer / further, or a duck held too long:
 * start over from this pose (the caller pushes it into the window). */
static void rebase(tr_track_t *t, int16_t cy, int16_t s, int16_t w)
{
	t->base_cy = (int32_t)cy * 16;
	t->base_s  = (int32_t)s * 16;
	t->base_w  = (int32_t)w * 16;
	t->based   = true;
	reset_duck(t);
}

/* The median shoulder width of the last three poses (fewer after a rebase):
 * one frame's width is too noisy for a +-10 % test (on 2026W36-0009 standing still
 * it spreads 41..66 px around ~52). */
static int16_t width_med3(const tr_track_t *t)
{
	int16_t v[3];
	uint8_t n = t->win_n;

	for (uint8_t k = 0; k < n; k++) {
		v[k] = t->win_w[(t->win_i + 3u - 1u - k) % 3u];
	}
	if (n < 3u) {
		return v[0];
	}
	int16_t lo = v[0] < v[1] ? v[0] : v[1], hi = v[0] < v[1] ? v[1] : v[0];

	return v[2] < lo ? lo : v[2] > hi ? hi : v[2];
}

static int32_t ema(int32_t base_q4, int16_t v)
{
	return base_q4 + ((int32_t)v * 16 - base_q4) / TR_TRACK_BASE_DIV;
}

void tr_track_init(tr_track_t *t, int16_t frame_h)
{
	t->frame_h     = frame_h;
	t->calibrated  = false;
	t->lost_frames = 0u;
	t->based       = false;
	t->last_seq    = 0u;
	reset_duck(t);
	tr_arms_reset(&t->arms);
}

void tr_track_calibrate(tr_track_t *t, tr_box_t b)
{
	if (!b.valid || b.confidence < TR_TRACK_MIN_CONF) {
		return;
	}
	/* "A player is there". NOT a stance baseline -- a box from the title
	 * screen can be anyone, standing anywhere; the first tracked frame seeds
	 * the rolling one. */
	t->lost_frames = 0u;
	t->based       = false;
	reset_duck(t);
	tr_arms_reset(&t->arms);
	t->calibrated = true;
}

bool tr_track_player_lost(const tr_track_t *t)
{
	return t->lost_frames >= TR_TRACK_LOST_LIMIT;
}

void tr_track_resync(tr_track_t *t)
{
	/* Only the arm edges are state the game can invalidate by dropping a
	 * tick; the baseline is derived from the camera feed itself and stays
	 * correct on its own. */
	tr_arms_reset(&t->arms);
}

/* a within pct % of b (both > 0). */
static bool near_pct(int32_t a, int32_t b, int32_t pct)
{
	int32_t d = a > b ? a - b : b - a;

	return d * 100 <= b * pct;
}

tr_intent_t tr_track_update(tr_track_t *t, tr_box_t b)
{
	tr_intent_t out = tr_intent_none();

	if (!b.valid || b.confidence < TR_TRACK_MIN_CONF || !t->calibrated) {
		if (t->lost_frames < 255u) {
			t->lost_frames++;
		}
		return out; /* source stays TR_INPUT_NONE: the caller must not act on this. */
	}
	if (t->lost_frames >= TR_TRACK_REACQ_FRAMES) {
		t->based = false; /* re-acquired: maybe someone else */
		tr_arms_reset(&t->arms);
	}
	t->lost_frames = 0u;
	out.source     = TR_INPUT_VISION;

	/* The duck state as it stands, repeat poses report it. */
	out.duck = t->duck_on > 0u;

	/* A pose already measured (main.c re-reads the last one every tick until
	 * the HP publishes anew): nothing below may count it twice (track.h). */
	if (b.seq != 0u && b.seq == t->last_seq) {
		return out;
	}
	t->last_seq = b.seq;

	/* The arms (arms.h): a one-tick lane step or jump on the pose that
	 * completes the gesture. */
	tr_arm_event_t arm = tr_arms_step(&t->arms, b.arm_raise);

	out.lane_delta = arm.lane_delta;
	out.jump       = arm.jump;

	/* The torso's scale and centre (track.h). Hips unseen: the shoulder width
	 * against the baseline's gives the scale. Either way, a scale under
	 * TR_TRACK_MIN_SCALE (a side-on player's collapsed width, hips mis-found
	 * a few px under the shoulders) measures nothing -- an arms-only frame. */
	int16_t s = b.h;

	if (s <= 0) {
		if (!t->based) {
			s = b.w; /* first sight without hips: the width is the unit */
		} else if (t->base_w > 0 && (int32_t)b.w * 16 * 2 >= t->base_w) {
			s = (int16_t)(t->base_s * b.w / t->base_w);
		}
	}
	if (s < TR_TRACK_MIN_SCALE) {
		return out;
	}

	int16_t cy = (int16_t)(TR_CAM_FLIP_Y && b.h <= 0 ? b.y - s / 2 : b.y + s / 2);

	if (TR_CAM_FLIP_Y) { /* see track.h's TR_CAM_FLIP_Y comment */
		cy = (int16_t)(t->frame_h - cy);
	}

	bool moved = !near_pct(s * 16, t->base_s, TR_TRACK_REBASE_PCT);

	if (t->duck_on > 0u && s * 16 < t->base_s && s * 16 * 2 >= t->base_s) {
		moved =
		    false; /* a held crouch leans the torso shorter: still the same player, down to half */
	}
	if (!t->based || moved) {
		rebase(t, cy, s, b.w);
		win_push(t, b.w);
		out.duck = false;
		return out;
	}

	int32_t bcy = t->base_cy / 16, bs = t->base_s / 16;

	win_push(t, b.w);

	int16_t w_med   = width_med3(t);
	bool width_kept = w_med > 0 && near_pct((int32_t)w_med * 16, t->base_w, TR_TRACK_SCALE_TOL_PCT);
	bool drop = width_kept && (cy - bcy) * 100 > bs * TR_TRACK_DUCK_K_PCT &&
	            s * 100 <= bs * (100 + TR_TRACK_SCALE_TOL_PCT);

	if (t->duck_on > 0u) {
		t->duck_on++;
		if (t->duck_on > TR_TRACK_DUCK_MAX) {
			/* Down this long is not dodging anything: a retreat the width
			 * test missed, or a new stance. It is the baseline now. */
			rebase(t, cy, s, b.w);
			win_push(t, b.w);
		} else if (t->duck_on > TR_TRACK_HOLD_MIN &&
		           (cy - bcy) * 100 < bs * (TR_TRACK_DUCK_K_PCT / 2)) {
			t->duck_on = 0u;
		}
	} else {
		t->duck_pend = drop ? (uint8_t)(t->duck_pend + 1u) : 0u;
		if (t->duck_pend >= TR_TRACK_DEBOUNCE) {
			t->duck_on   = 1u;
			t->duck_pend = 0u;
		} else if (t->duck_pend == 0u) {
			/* Neutral: the baseline follows the player. */
			t->base_cy = ema(t->base_cy, cy);
			t->base_s  = ema(t->base_s, s);
			if ((int32_t)b.w * 16 * 2 >= t->base_w) {
				t->base_w = ema(t->base_w, b.w);
			}
		}
	}
	out.duck = t->duck_on > 0u;
	return out;
}
