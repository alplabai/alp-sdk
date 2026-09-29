/* src/vision/track.c */
#include "track.h"

#include <stdlib.h>

/*
 * Body tracking, not gesture classification: WHERE the player is beats WHICH of
 * five poses they are striking, both in robustness and in code size.  All three
 * controls fall out of one bounding box.
 *
 * Everything here is pure: no camera, no SDK, no allocation.  That is what lets
 * the whole control scheme be tested on the host before touching hardware.
 */

static uint8_t band_of(const tr_track_t *t, int16_t centre)
{
	/* Hysteresis: the edge you must cross depends on the lane you are in, so a
	 * player standing on a boundary does not flicker between two lanes. */
	int16_t e0 = t->lane_edges[0];
	int16_t e1 = t->lane_edges[1];

	if (t->lane == 0) {
		return (centre > e0 + TR_TRACK_HYST_PX) ? ((centre > e1 + TR_TRACK_HYST_PX) ? 2u : 1u) : 0u;
	}
	if (t->lane == 2u) {
		return (centre < e1 - TR_TRACK_HYST_PX) ? ((centre < e0 - TR_TRACK_HYST_PX) ? 0u : 1u) : 2u;
	}
	/* Leaving the centre lane uses the plain edge, no margin: the margin only
	 * guards the RETURN trip (above), which is what stops the flicker. */
	if (centre < e0) {
		return 0u;
	}
	if (centre > e1) {
		return 2u;
	}
	return 1u;
}

static void reset_gestures(tr_track_t *t)
{
	t->win_n = t->win_i = t->settle = 0u;
	t->jump_pend = t->duck_pend = t->jump_on = t->duck_on = t->land_cooldown = 0u;
}

/* Append one measured pose to the jump window. */
static void win_push(tr_track_t *t, int16_t cy, int16_t sho, int16_t hip, int16_t h, int16_t w)
{
	t->win_w[t->win_i]   = w;
	t->win_cy[t->win_i]  = cy;
	t->win_sho[t->win_i] = sho;
	t->win_hip[t->win_i] = hip;
	t->win_h[t->win_i]   = h;
	t->win_i             = (uint8_t)((t->win_i + 1u) % TR_TRACK_WIN);
	if (t->win_n < TR_TRACK_WIN) {
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
	reset_gestures(t);
}

/* The median shoulder width of the last three poses (fewer after a rebase):
 * one frame's width is too noisy for a +-10 % test (on 2026W36-0009 standing still
 * it spreads 41..66 px around ~52). */
static int16_t width_med3(const tr_track_t *t)
{
	int16_t v[3];
	uint8_t n = t->win_n < 3u ? t->win_n : 3u;

	for (uint8_t k = 0; k < n; k++) {
		v[k] = t->win_w[(t->win_i + TR_TRACK_WIN - 1u - k) % TR_TRACK_WIN];
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

void tr_track_init(tr_track_t *t, int16_t frame_w, int16_t frame_h)
{
	t->lane_edges[0] = (int16_t)(frame_w / 3);
	t->lane_edges[1] = (int16_t)((frame_w * 2) / 3);
	t->frame_w       = frame_w;
	t->frame_h       = frame_h;
	t->calibrated    = false;
	t->lane          = 1u;
	t->lost_frames   = 0u;
	t->based         = false;
	t->last_seq      = 0u;
	reset_gestures(t);
}

void tr_track_calibrate(tr_track_t *t, tr_box_t b, int16_t frame_w)
{
	if (!b.valid || b.confidence < TR_TRACK_MIN_CONF) {
		return;
	}
	/* "A player is there": lanes and the centre-lane start. NOT a stance
	 * baseline -- a box from the title screen can be anyone, standing
	 * anywhere; the first tracked frame seeds the rolling one. */
	t->lane_edges[0] = (int16_t)(frame_w / 3);
	t->lane_edges[1] = (int16_t)((frame_w * 2) / 3);
	t->frame_w       = frame_w;
	t->lane          = 1u;
	t->lost_frames   = 0u;
	t->based         = false;
	reset_gestures(t);
	t->calibrated = true;
}

bool tr_track_player_lost(const tr_track_t *t)
{
	return t->lost_frames >= TR_TRACK_LOST_LIMIT;
}

void tr_track_resync(tr_track_t *t, uint8_t lane)
{
	/* Only the lane belief is external state the game can invalidate by
	 * dropping a tick; everything else here is derived from the camera feed
	 * itself and stays correct on its own. */
	t->lane = lane;
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
	}
	t->lost_frames = 0u;
	out.source     = TR_INPUT_VISION;

	int16_t centre = (int16_t)(b.x + b.w / 2);

	if (TR_CAM_MIRROR_X) {
		centre = (int16_t)(t->frame_w - 1 - centre); /* see track.h's TR_CAM_MIRROR_X comment */
	}

	uint8_t want = band_of(t, centre);

	if (want != t->lane) {
		out.lane_delta = (want > t->lane) ? +1 : -1;
		/* Step one lane per frame so a big sideways stride cannot teleport. */
		t->lane = (uint8_t)(t->lane + (want > t->lane ? 1 : -1));
	}

	/* The gesture state as it stands, lane-only / repeat poses report it. */
	out.jump = t->jump_on > 0u;
	out.duck = t->duck_on > 0u;

	/* A pose already measured (main.c re-reads the last one every tick until
	 * the HP publishes anew): nothing below may count it twice (track.h). */
	if (b.seq != 0u && b.seq == t->last_seq) {
		return out;
	}
	t->last_seq = b.seq;

	/* The torso's scale and centre (track.h). Hips unseen: the shoulder width
	 * against the baseline's gives the scale. Either way, a scale under
	 * TR_TRACK_MIN_SCALE (a side-on player's collapsed width, hips mis-found
	 * a few px under the shoulders) measures nothing -- a lane-only frame. */
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

	int16_t cy  = (int16_t)(TR_CAM_FLIP_Y && b.h <= 0 ? b.y - s / 2 : b.y + s / 2);
	int16_t sho = b.y, hip = (int16_t)(b.y + b.h);

	if (TR_CAM_FLIP_Y) { /* see track.h's TR_CAM_FLIP_Y comment */
		cy  = (int16_t)(t->frame_h - cy);
		sho = (int16_t)(t->frame_h - sho);
		hip = (int16_t)(t->frame_h - hip);
	}

	bool moved = !near_pct(s * 16, t->base_s, TR_TRACK_REBASE_PCT);

	if (t->duck_on > 0u && s * 16 < t->base_s && s * 16 * 2 >= t->base_s) {
		moved =
		    false; /* a held crouch leans the torso shorter: still the same player, down to half */
	}
	if (!t->based || moved) {
		bool mid_stream = t->based;

		rebase(t, cy, s, b.w);
		/* Re-seeded mid-stream (much closer or further: still moving):
		 * the new base_w is one noisy pose, so no jump for a window. */
		t->settle = mid_stream ? TR_TRACK_WIN : 0u;
		win_push(t, cy, sho, hip, b.h, b.w);
		out.jump = out.duck = false;
		return out;
	}

	int32_t bcy = t->base_cy / 16, bs = t->base_s / 16;

	/* The window's low point (largest cy): where a jump would have started. */
	int16_t low_cy = cy, low_sho = sho, low_hip = hip, low_h = b.h;

	for (uint8_t k = 0; k < t->win_n; k++) {
		if (t->win_cy[k] > low_cy) {
			low_cy = t->win_cy[k], low_sho = t->win_sho[k], low_hip = t->win_hip[k],
			low_h = t->win_h[k];
		}
	}
	win_push(t, cy, sho, hip, b.h, b.w);

	int32_t k_px = bs * TR_TRACK_JUMP_K_PCT; /* x100 */
	/* The torso length change: against the low frame (local) and against
	 * the baseline, whichever is larger -- one noisy low frame alone lets
	 * an approach's growth through (track.h). */
	int32_t dh = abs(b.h - low_h), dh_base = abs(b.h - (int32_t)bs);

	dh = dh > dh_base ? dh : dh_base;
	if (t->settle > 0u) {
		t->settle--;
	}
	int16_t w_med   = width_med3(t);
	bool width_kept = w_med > 0 && near_pct((int32_t)w_med * 16, t->base_w, TR_TRACK_SCALE_TOL_PCT);
	bool rise = t->settle == 0u && b.h > 0 && low_h > 0 && width_kept && (bcy - cy) * 100 > k_px &&
	            (low_sho - sho) * 100 > k_px && (low_hip - hip) * 100 > k_px &&
	            dh * 100 < (low_cy - cy) * TR_TRACK_JUMP_DH_PCT;
	bool drop = width_kept && (cy - bcy) * 100 > bs * TR_TRACK_DUCK_K_PCT &&
	            s * 100 <= bs * (100 + TR_TRACK_SCALE_TOL_PCT);

	if (t->jump_on > 0u) {
		t->jump_on++;
		bool down = (t->jump_ref_cy - cy) * 100 < bs * (TR_TRACK_JUMP_K_PCT / 2);

		if ((t->jump_on > TR_TRACK_HOLD_MIN && down) || t->jump_on > TR_TRACK_JUMP_MAX) {
			t->jump_on       = 0u;
			t->land_cooldown = TR_TRACK_LAND_COOLDOWN_FRAMES;
		}
	} else if (t->duck_on > 0u) {
		t->duck_on++;
		if (t->duck_on > TR_TRACK_DUCK_MAX) {
			/* Down this long is not dodging anything: a retreat the width
			 * test missed, or a new stance. It is the baseline now. */
			rebase(t, cy, s, b.w);
			win_push(t, cy, sho, hip, b.h, b.w);
		} else if (t->duck_on > TR_TRACK_HOLD_MIN &&
		           (cy - bcy) * 100 < bs * (TR_TRACK_DUCK_K_PCT / 2)) {
			t->duck_on = 0u;
		}
	} else {
		t->jump_pend = rise ? (uint8_t)(t->jump_pend + 1u) : 0u;
		t->duck_pend = (drop && t->land_cooldown == 0u) ? (uint8_t)(t->duck_pend + 1u) : 0u;
		if (t->jump_pend >= TR_TRACK_DEBOUNCE) {
			t->jump_on     = 1u;
			t->jump_ref_cy = low_cy;
			t->jump_pend = t->duck_pend = 0u;
		} else if (t->duck_pend >= TR_TRACK_DEBOUNCE) {
			t->duck_on   = 1u;
			t->jump_pend = t->duck_pend = 0u;
		} else if (t->land_cooldown > 0u) {
			t->land_cooldown--;
		} else if (t->jump_pend == 0u && t->duck_pend == 0u) {
			/* Neutral: the baseline follows the player. */
			t->base_cy = ema(t->base_cy, cy);
			t->base_s  = ema(t->base_s, s);
			if ((int32_t)b.w * 16 * 2 >= t->base_w) {
				t->base_w = ema(t->base_w, b.w);
			}
		}
	}
	out.jump = t->jump_on > 0u;
	out.duck = t->duck_on > 0u;
	return out;
}
