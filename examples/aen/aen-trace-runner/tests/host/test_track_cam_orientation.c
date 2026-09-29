/* tests/host/test_track_cam_orientation.c
 *
 * Coverage for TR_CAM_MIRROR_X and TR_CAM_FLIP_Y in the `1` state --
 * whole-branch fix round B, B2: neither constant had any test coverage in
 * either state before this. tests/host/test_track.c already covers the
 * default `0` state extensively (every box in it is fed and expected back
 * un-mirrored, un-flipped); this file is compiled by tests/host/runner.sh
 * with `-DTR_CAM_MIRROR_X=1 -DTR_CAM_FLIP_Y=1` (see track.h's #ifndef
 * guards) so it exercises the SAME track.c logic with both transforms
 * live, not a second implementation.
 */
#include <assert.h>
#include "../../src/vision/track.h"

static tr_box_t box(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t conf)
{
	return (tr_box_t){ .x = x, .y = y, .w = w, .h = h, .confidence = conf, .valid = true };
}

#if !TR_CAM_MIRROR_X || !TR_CAM_FLIP_Y
#error \
    "this file must be compiled with -DTR_CAM_MIRROR_X=1 -DTR_CAM_FLIP_Y=1 (see tests/host/runner.sh)"
#endif

int main(void)
{
	tr_track_t t;

	/*
	 * TR_CAM_MIRROR_X: the exact box tests/host/test_track.c feeds for
	 * "stepping to the player's left" (a small image x) reports
	 * lane_delta == -1 with the transform OFF. With it ON, the same box
	 * must mirror to the OPPOSITE side of the frame and report +1 instead
	 * -- proving the mirror is live, not just present as a dead constant.
	 */
	tr_track_init(&t, 640, 400);
	tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
	tr_intent_t i = tr_track_update(&t, box(40, 100, 100, 300, 90));

	assert(i.lane_delta == +1); /* test_track.c's un-mirrored case asserts -1 for this exact box */

	/*
	 * TR_CAM_FLIP_Y, jump: under a real 180-degree rotation a physical jump
	 * moves the torso centre DOWN the raw frame at constant length; the
	 * centre is what track.c mirrors. Seed, then the centre 0.4 s further
	 * down the raw frame for two frames: a jump.
	 */
	tr_track_init(&t, 640, 400);
	tr_track_calibrate(&t, box(270, 100, 100, 100, 90), 640);
	(void)tr_track_update(&t, box(270, 100, 100, 100, 90));
	(void)tr_track_update(&t, box(270, 140, 100, 100, 90));
	i = tr_track_update(&t, box(270, 140, 100, 100, 90));
	assert(i.jump && !i.duck);

	/* TR_CAM_FLIP_Y, duck: the centre UP the raw frame. */
	tr_track_init(&t, 640, 400);
	tr_track_calibrate(&t, box(270, 100, 100, 100, 90), 640);
	(void)tr_track_update(&t, box(270, 100, 100, 100, 90));
	(void)tr_track_update(&t, box(270, 60, 100, 100, 90));
	i = tr_track_update(&t, box(270, 60, 100, 100, 90));
	assert(i.duck && !i.jump);

	/* TR_CAM_FLIP_Y, no hips (h = 0): the raw shoulders are the torso's
	 * BOTTOM, so the centre is s/2 ABOVE them before the mirror. A duck
	 * (shoulders up the raw frame by 0.4 s) still reads as a duck. */
	tr_track_init(&t, 640, 400);
	tr_track_calibrate(&t, box(270, 300, 100, 0, 90), 640);
	(void)tr_track_update(&t, box(270, 300, 100, 0, 90));
	(void)tr_track_update(&t, box(270, 260, 100, 0, 90));
	i = tr_track_update(&t, box(270, 260, 100, 0, 90));
	assert(i.duck && !i.jump);

	return 0;
}
