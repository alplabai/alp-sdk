/* tests/host/test_track.c */
#include <assert.h>
#include "../../src/vision/track.h"

static tr_box_t box(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t conf)
{
	return (tr_box_t){ .x = x, .y = y, .w = w, .h = h, .confidence = conf, .valid = true };
}

int main(void)
{
	tr_track_t t;

	/* Calibration: standing centre at 640 wide sets the stance baseline. */
	tr_track_init(&t, 640, 400);
	tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
	assert(t.calibrated);

	/* Standing still in the centre asks for no lane change. */
	tr_intent_t i = tr_track_update(&t, box(270, 100, 100, 300, 90));
	assert(i.lane_delta == 0 && !i.jump && !i.duck);
	assert(i.source == TR_INPUT_VISION);

	/* Stepping to the player's left moves one lane, once -- not every frame. */
	i = tr_track_update(&t, box(40, 100, 100, 300, 90));
	assert(i.lane_delta == -1);
	i = tr_track_update(&t, box(40, 100, 100, 300, 90));
	assert(i.lane_delta == 0);

	/* Stepping right from there comes back to centre, then right again. */
	i = tr_track_update(&t, box(270, 100, 100, 300, 90));
	assert(i.lane_delta == +1);
	i = tr_track_update(&t, box(500, 100, 100, 300, 90));
	assert(i.lane_delta == +1);

	/* Hysteresis: a small wobble around a band edge must not flip lanes. */
	tr_track_init(&t, 640, 400);
	tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
	int16_t edge = t.lane_edges[0];
	i            = tr_track_update(&t, box((int16_t)(edge - 52), 100, 100, 300, 90));
	assert(i.lane_delta == -1);
	i = tr_track_update(&t, box((int16_t)(edge - 46), 100, 100, 300, 90));
	assert(i.lane_delta == 0);

	/* A jump (track.h): the torso box -- shoulder-mid y, torso length h, so
	 * the centre is y + h/2 -- lifts 0.4 of its length at constant length.
	 * The first tracked frame seeds the baseline (calibration does not), and
	 * the lift must hold TR_TRACK_DEBOUNCE frames. */
	tr_track_init(&t, 640, 400);
	tr_track_calibrate(&t, box(270, 60, 100, 300, 90), 640); /* whoever the title screen saw */
	i = tr_track_update(&t, box(270, 200, 100, 100, 90));    /* seeds the baseline: centre 250 */
	assert(!i.jump && !i.duck);
	i = tr_track_update(&t, box(270, 160, 100, 100, 90));
	assert(!i.jump && !i.duck); /* one frame is not enough */
	i = tr_track_update(&t, box(270, 160, 100, 100, 90));
	assert(i.jump && !i.duck);

	/* A crouch: the torso centre drops 0.4 of its length. */
	tr_track_init(&t, 640, 400);
	tr_track_calibrate(&t, box(270, 200, 100, 100, 90), 640);
	(void)tr_track_update(&t, box(270, 200, 100, 100, 90));
	(void)tr_track_update(&t, box(270, 240, 100, 100, 90));
	i = tr_track_update(&t, box(270, 240, 100, 100, 90));
	assert(i.duck && !i.jump);

	/* The same box grown by half (walking closer): neither, a new baseline. */
	for (int k = 0; k < 10; k++) {
		i = tr_track_update(&t, box(270, 150, 100, 150, 90));
		assert(!i.duck && !i.jump);
	}

	/* A whole-body box (detect.c's, fed with h = 0 by main.c): the width is
	 * the scale, the top the position -- a crouch lowers the top, keeps the
	 * width. */
	tr_track_init(&t, 640, 400);
	tr_track_calibrate(&t, box(270, 100, 100, 0, 90), 640);
	(void)tr_track_update(&t, box(270, 100, 100, 0, 90));
	(void)tr_track_update(&t, box(270, 190, 100, 0, 90));
	i = tr_track_update(&t, box(270, 190, 100, 0, 90));
	assert(i.duck && !i.jump);

	/* Low confidence is ignored entirely. */
	tr_track_init(&t, 640, 400);
	tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
	i = tr_track_update(&t, box(40, 100, 100, 300, 10));
	assert(i.lane_delta == 0 && i.source == TR_INPUT_NONE);

	/* Losing the player for long enough reports it, so the game can pause. */
	tr_track_init(&t, 640, 400);
	tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
	for (int k = 0; k < TR_TRACK_LOST_LIMIT + 1; k++) {
		tr_box_t none = { .valid = false };

		(void)tr_track_update(&t, none);
	}
	assert(tr_track_player_lost(&t));

	/* A jump's landing ends in a brief knees-bent crouch that must not
	 * register as a duck for TR_TRACK_LAND_COOLDOWN_FRAMES after the jump. */
	tr_track_init(&t, 640, 400);
	tr_track_calibrate(&t, box(270, 200, 100, 100, 90), 640);
	(void)tr_track_update(&t, box(270, 200, 100, 100, 90));
	for (int k = 0; k < TR_TRACK_HOLD_MIN + 1; k++) {
		(void)tr_track_update(&t, box(270, 160, 100, 100, 90)); /* the jump itself */
	}
	assert(t.jump_on > 0u);
	i = tr_track_update(&t, box(270, 240, 100, 100, 90)); /* touch-down: the jump ends */
	assert(!i.jump && !i.duck);
	for (int k = 0; k < TR_TRACK_LAND_COOLDOWN_FRAMES; k++) {
		i = tr_track_update(&t, box(270, 240, 100, 100, 90)); /* landing crouch */
		assert(!i.jump && !i.duck);
	}
	/* Back to full height: no duck lingers. */
	for (int k = 0; k < 5; k++) {
		i = tr_track_update(&t, box(270, 200, 100, 100, 90));
		assert(!i.jump && !i.duck);
	}

	/* A player whose torso shrinks gradually (stepping back) never latches a
	 * duck: the baseline chases the drift, or re-seeds past +-30 %. From 100
	 * down to 50, one pixel every three frames, the shoulders sinking with it. */
	tr_track_init(&t, 640, 400);
	tr_track_calibrate(&t, box(270, 200, 100, 100, 90), 640);
	int16_t h = 100;
	for (int k = 0; k < 200; k++) {
		if (k % 3 == 0 && h > 50) {
			h--;
		}
		i = tr_track_update(&t, box(270, (int16_t)(200 + (100 - h)), 100, h, 90));
		assert(!i.duck && !i.jump);
	}

	/* Resync: after the caller drops a tick (pause, game-over, reset) without
	 * applying the tracker's delta, tr_track_resync() must force the lane
	 * belief back into agreement so the next real delta is computed from where
	 * the game actually is, not from stale tracker state. */
	tr_track_init(&t, 640, 400);
	tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
	i = tr_track_update(&t, box(40, 100, 100, 300, 90)); /* step left: tracker believes lane 0 */
	assert(i.lane_delta == -1 && t.lane == 0);
	tr_track_resync(&t, 1); /* game says the player is really back in the centre lane */
	assert(t.lane == 1);
	i = tr_track_update(&t, box(270, 100, 100, 300, 90)); /* standing centre now asks for no move */
	assert(i.lane_delta == 0);
	return 0;
}
