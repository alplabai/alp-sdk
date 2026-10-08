/* tests/host/test_track_cam_orientation.c
 *
 * Coverage for TR_CAM_FLIP_Y in the `1` state. tests/host/test_track.c
 * already covers the default `0` state extensively (every box in it is fed
 * and expected back un-flipped); this file is compiled by tests/host/runner.sh
 * with `-DTR_CAM_FLIP_Y=1` (see track.h's #ifndef guard) so it exercises the
 * SAME track.c / pose.c logic with the transform live, not a second
 * implementation.
 */
#include <assert.h>
#include "../../src/vision/pose.h"
#include "../../src/vision/track.h"

static tr_box_t box(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t conf)
{
	return (tr_box_t){ .x = x, .y = y, .w = w, .h = h, .confidence = conf, .valid = true };
}

#if !TR_CAM_FLIP_Y
#error "this file must be compiled with -DTR_CAM_FLIP_Y=1 (see tests/host/runner.sh)"
#endif

int main(void)
{
	tr_track_t  t;
	tr_intent_t i;

	/*
	 * TR_CAM_FLIP_Y, arms: the image is upside down, so a RAISED wrist is
	 * BELOW its shoulder in it. pose.c reads it that way round: the same
	 * keypoints a normal camera reads as a lowered arm are a raised arm here.
	 */
	{
		tr_pose_t p = { 0 };

		for (int k = 0; k < TR_POSE_KP; k++) {
			p.kp[k] = (tr_kp_t){ 200, 200, 200 };
		}
		p.kp[TR_KP_LSHO] = (tr_kp_t){ 240, 200, 200 };
		p.kp[TR_KP_RSHO] = (tr_kp_t){ 160, 200, 200 };
		p.kp[TR_KP_LWRI] = (tr_kp_t){ 280, 280, 200 }; /* 100 % of the shoulder width BELOW */
		p.kp[TR_KP_RWRI] = (tr_kp_t){ 120, 120, 200 }; /* 100 % ABOVE: lowered, flipped */

		tr_box_t b = tr_pose_box(&p);

		/* Default TR_CAM_MIRROR=1: the label-LEFT pair sits at larger x, so it is the
		 * player's RIGHT arm (pose.c); its wrist is below the shoulder in the
		 * flipped image, i.e. raised by one shoulder width. */
		assert(b.arm_raise[TR_ARM_RIGHT] == 100 && b.arm_raise[TR_ARM_LEFT] == -100);
	}

	/* TR_CAM_FLIP_Y, duck: the centre UP the raw frame. */
	tr_track_init(&t, 400);
	tr_track_calibrate(&t, box(270, 100, 100, 100, 90));
	(void)tr_track_update(&t, box(270, 100, 100, 100, 90));
	(void)tr_track_update(&t, box(270, 60, 100, 100, 90));
	i = tr_track_update(&t, box(270, 60, 100, 100, 90));
	assert(i.duck && !i.jump);

	/* TR_CAM_FLIP_Y, no hips (h = 0): the raw shoulders are the torso's
	 * BOTTOM, so the centre is s/2 ABOVE them before the mirror. A duck
	 * (shoulders up the raw frame by 0.4 s) still reads as a duck. */
	tr_track_init(&t, 400);
	tr_track_calibrate(&t, box(270, 300, 100, 0, 90));
	(void)tr_track_update(&t, box(270, 300, 100, 0, 90));
	(void)tr_track_update(&t, box(270, 260, 100, 0, 90));
	i = tr_track_update(&t, box(270, 260, 100, 0, 90));
	assert(i.duck && !i.jump);

	return 0;
}
