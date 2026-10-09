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

	/* Calibration: "a player is there"; the first tracked pose seeds the baseline. */
	tr_track_init(&t, 400);
	tr_track_calibrate(&t, box(270, 100, 100, 300, 90));
	assert(t.calibrated);

	/* Standing still asks for nothing. */
	tr_intent_t i = tr_track_update(&t, box(270, 100, 100, 300, 90));
	assert(i.lane_delta == 0 && !i.jump && !i.duck);
	assert(i.source == TR_INPUT_VISION);

	/* Where the player stands is no lane: a box far to either side asks for
	 * nothing (the lane is the arms', arms.h). */
	i = tr_track_update(&t, box(40, 100, 100, 300, 90));
	assert(i.lane_delta == 0 && !i.jump);
	i = tr_track_update(&t, box(500, 100, 100, 300, 90));
	assert(i.lane_delta == 0 && !i.jump);

	/* A torso lifted 0.4 of its length is no jump (that is both arms up). */
	tr_track_init(&t, 400);
	tr_track_calibrate(&t, box(270, 60, 100, 300, 90)); /* whoever the title screen saw */
	i = tr_track_update(&t, box(270, 200, 100, 100, 90));
	i = tr_track_update(&t, box(270, 160, 100, 100, 90));
	i = tr_track_update(&t, box(270, 160, 100, 100, 90));
	assert(!i.jump && !i.duck);

	/* The arms through the box: the left arm's raise level makes one lane
	 * step on the pose that completes the settle window. */
	tr_track_init(&t, 400);
	tr_track_calibrate(&t, box(270, 100, 100, 300, 90));
	{
		tr_box_t up    = box(270, 100, 100, 300, 90);
		int      steps = 0;

		up.arm_raise[TR_ARM_LEFT] = 100;
		for (int k = 0; k < 20; k++) {
			i = tr_track_update(&t, k == 0 ? box(270, 100, 100, 300, 90) : up);
			steps += i.lane_delta == -1;
			assert(i.lane_delta >= -1 && i.lane_delta <= 0 && !i.jump);
		}
		assert(steps == 1);
	}

	/* A crouch: the torso centre drops 0.4 of its length. */
	tr_track_init(&t, 400);
	tr_track_calibrate(&t, box(270, 200, 100, 100, 90));
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
	tr_track_init(&t, 400);
	tr_track_calibrate(&t, box(270, 100, 100, 0, 90));
	(void)tr_track_update(&t, box(270, 100, 100, 0, 90));
	(void)tr_track_update(&t, box(270, 190, 100, 0, 90));
	i = tr_track_update(&t, box(270, 190, 100, 0, 90));
	assert(i.duck && !i.jump);

	/* Low confidence is ignored entirely, arms included. */
	tr_track_init(&t, 400);
	tr_track_calibrate(&t, box(270, 100, 100, 300, 90));
	{
		tr_box_t weak = box(40, 100, 100, 300, 10);

		weak.arm_raise[TR_ARM_LEFT] = 100;
		i                           = tr_track_update(&t, weak);
	}
	assert(i.lane_delta == 0 && i.source == TR_INPUT_NONE);

	/* Losing the player for long enough reports it, so the game can pause. */
	tr_track_init(&t, 400);
	tr_track_calibrate(&t, box(270, 100, 100, 300, 90));
	for (int k = 0; k < TR_TRACK_LOST_LIMIT + 1; k++) {
		tr_box_t none = { .valid = false };

		(void)tr_track_update(&t, none);
	}
	assert(tr_track_player_lost(&t));

	/* A player whose torso shrinks gradually (stepping back) never latches a
	 * duck: the baseline chases the drift, or re-seeds past +-30 %. From 100
	 * down to 50, one pixel every three frames, the shoulders sinking with it. */
	tr_track_init(&t, 400);
	tr_track_calibrate(&t, box(270, 200, 100, 100, 90));
	int16_t h = 100;
	for (int k = 0; k < 200; k++) {
		if (k % 3 == 0 && h > 50) {
			h--;
		}
		i = tr_track_update(&t, box(270, (int16_t)(200 + (100 - h)), 100, h, 90));
		assert(!i.duck && !i.jump);
	}

	/* Resync: after the caller drops a tick (pause, game-over, reset) an arm
	 * that is still up must not turn into a lane step: it is spent until it
	 * has been lowered. */
	tr_track_init(&t, 400);
	tr_track_calibrate(&t, box(270, 100, 100, 300, 90));
	{
		tr_box_t up = box(270, 100, 100, 300, 90);

		up.arm_raise[TR_ARM_RIGHT] = 100;
		(void)tr_track_update(&t, box(270, 100, 100, 300, 90));
		for (int k = 0; k < 10; k++) {
			(void)tr_track_update(&t, up);
		}
		tr_track_resync(&t);
		for (int k = 0; k < 10; k++) {
			i = tr_track_update(&t, up);
			assert(i.lane_delta == 0);
		}
	}
	return 0;
}
