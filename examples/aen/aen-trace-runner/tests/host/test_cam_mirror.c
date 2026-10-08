/* tests/host/test_cam_mirror.c -- the selfie mirror (src/vision/cam_rot.h
 * TR_CAM_MIRROR): the sensor bit tr_cam_mirror_reg() names, followed by
 * the software rotation tr_cam_rot_src(), must mirror the UPRIGHT view
 * left/right -- on every pixel, at every rotation -- and the arm controls
 * must read the player's physical left arm as LEFT through that whole chain.
 *
 * The sensor is modelled by what its flip bits do to the raw readout: VFLIP
 * (0x3820 bit 2) reverses the rows, HMIRROR (0x3821 bit 2) the columns. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "../../src/vision/cam_rot.h"
#include "../../src/vision/pose.h"
#include "../../src/vision/track.h"

#define W TR_CAM_SENSOR_W
#define H TR_CAM_SENSOR_H

/* The scene pixel the sensor outputs at raw (sx, sy) with `flip_reg`'s
 * flip bit set (0: none). A unique id per scene pixel: y * W + x. */
static int sensor(uint16_t flip_reg, int sx, int sy)
{
	if (flip_reg == TR_OV9281_REG_TIMING_FORMAT1) {
		sy = H - 1 - sy;
	} else if (flip_reg == TR_OV9281_REG_TIMING_FORMAT2) {
		sx = W - 1 - sx;
	}
	return sy * W + sx;
}

/* The upright pixel (ux, uy) as the HP/A32 build it: sensor, then rotation. */
static int upright(int rot, uint16_t flip_reg, int ux, int uy)
{
	int sx, sy;

	tr_cam_rot_src(rot, W, H, ux, uy, &sx, &sy);
	return sensor(flip_reg, sx, sy);
}

/* true when flipping `flip_reg` makes the upright view the exact left/right
 * mirror of the unflipped one, every pixel. */
static int is_selfie(int rot, uint16_t flip_reg)
{
	int uw = TR_CAM_UP_W(rot), uh = TR_CAM_UP_H(rot);

	for (int uy = 0; uy < uh; uy++) {
		for (int ux = 0; ux < uw; ux++) {
			if (upright(rot, flip_reg, ux, uy) != upright(rot, 0u, uw - 1 - ux, uy)) {
				return 0;
			}
		}
	}
	return 1;
}

/* Where the scene pixel `id` lands in the upright view. */
static void find(int rot, uint16_t flip_reg, int id, int *ux, int *uy)
{
	for (*uy = 0; *uy < TR_CAM_UP_H(rot); (*uy)++) {
		for (*ux = 0; *ux < TR_CAM_UP_W(rot); (*ux)++) {
			if (upright(rot, flip_reg, *ux, *uy) == id) {
				return;
			}
		}
	}
	assert(0 && "pixel not in the upright view");
}

/* A player facing the camera in the UNMIRRORED upright frame (uw x uh), as
 * MoveNet would report them: the label-LEFT keypoints sit at larger x. The
 * torso is centred at (uw / 2, uh / 3) with a shoulder width of uh / 5 (the
 * frame is 400 or 640 wide, 640 or 400 tall: the figure fits all of them);
 * a raised wrist is 120 % of the shoulder width above its shoulder, a lowered
 * one 80 % below it. `*_up` is the player's own PHYSICAL arm. */
static tr_pose_t player(int uw, int uh, bool left_up, bool right_up)
{
	tr_pose_t p  = { 0 };
	int       sw = uh / 5, cx = uw / 2, cy = uh / 3;

	for (int k = 0; k < TR_POSE_KP; k++) {
		p.kp[k] = (tr_kp_t){ (int16_t)cx, (int16_t)cy, 200 };
	}
	p.kp[TR_KP_LSHO] = (tr_kp_t){ (int16_t)(cx + sw / 2), (int16_t)cy, 200 };
	p.kp[TR_KP_RSHO] = (tr_kp_t){ (int16_t)(cx - sw / 2), (int16_t)cy, 200 };
	p.kp[TR_KP_LHIP] = (tr_kp_t){ (int16_t)(cx + sw / 3), (int16_t)(cy + sw * 3 / 2), 200 };
	p.kp[TR_KP_RHIP] = (tr_kp_t){ (int16_t)(cx - sw / 3), (int16_t)(cy + sw * 3 / 2), 200 };
	p.kp[TR_KP_LWRI] = (tr_kp_t){ (int16_t)(cx + sw),
		                          (int16_t)(left_up ? cy - sw * 12 / 10 : cy + sw * 8 / 10),
		                          200 };
	p.kp[TR_KP_RWRI] = (tr_kp_t){ (int16_t)(cx - sw),
		                          (int16_t)(right_up ? cy - sw * 12 / 10 : cy + sw * 8 / 10),
		                          200 };
	return p;
}

/* The same pose as the (possibly flipped) sensor and rotation deliver it:
 * every keypoint is the scene pixel it names, found again in the upright view
 * of `flip_reg`. MoveNet then labels what it SEES, so the pair with the
 * larger shoulder x is the "left" one whatever body it belongs to. */
static tr_pose_t as_delivered(tr_pose_t p, int rot, uint16_t flip_reg)
{
	for (int k = 0; k < TR_POSE_KP; k++) {
		int ux, uy;

		find(rot, flip_reg, upright(rot, 0u, p.kp[k].x, p.kp[k].y), &ux, &uy);
		p.kp[k].x = (int16_t)ux;
		p.kp[k].y = (int16_t)uy;
	}
	if (p.kp[TR_KP_LSHO].x < p.kp[TR_KP_RSHO].x) {
		for (int k = TR_KP_LEYE; k < TR_POSE_KP; k += 2) {
			tr_kp_t tmp = p.kp[k];

			p.kp[k]     = p.kp[k + 1];
			p.kp[k + 1] = tmp;
		}
	}
	return p;
}

/* The intent after `n` poses of the player holding the given arms up. */
static tr_intent_t hold(tr_track_t *t, int rot, uint16_t flip, bool l, bool r, int n)
{
	tr_pose_t   p   = as_delivered(player(TR_CAM_UP_W(rot), TR_CAM_UP_H(rot), l, r), rot, flip);
	tr_intent_t sum = tr_intent_none();

	for (int k = 0; k < n; k++) {
		tr_intent_t in = tr_track_update(t, tr_pose_box(&p));

		sum.lane_delta = (int8_t)(sum.lane_delta + in.lane_delta);
		sum.jump       = sum.jump || in.jump;
	}
	return sum;
}

int main(void)
{
	/* 1. The derivation: at 90 and 270 the sensor's VFLIP is the selfie
	 * mirror, its HMIRROR is not (that turns the view upside down); at 0 it
	 * is the other way round. tr_cam_mirror_reg() picks the right one. */
	assert(tr_cam_mirror_reg(90) == TR_OV9281_REG_TIMING_FORMAT1);
	assert(tr_cam_mirror_reg(270) == TR_OV9281_REG_TIMING_FORMAT1);
	assert(tr_cam_mirror_reg(0) == TR_OV9281_REG_TIMING_FORMAT2);
	for (int rot = 0; rot <= 270; rot += 90) {
		if (rot == 180) {
			continue;
		}
		assert(is_selfie(rot, tr_cam_mirror_reg(rot)));
		assert(!is_selfie(rot,
		                  tr_cam_mirror_reg(rot) == TR_OV9281_REG_TIMING_FORMAT1
		                      ? TR_OV9281_REG_TIMING_FORMAT2
		                      : TR_OV9281_REG_TIMING_FORMAT1));
	}

	/* 2. A marked corner, the bench's TR_CAM_ROTATE=90: the scene's raw
	 * top-left pixel shows at the upright TOP-RIGHT unmirrored (the image
	 * turned clockwise), and a selfie puts it at the TOP-LEFT. */
	{
		int ux, uy;

		find(90, 0u, 0, &ux, &uy);
		assert(ux == H - 1 && uy == 0);
		find(90, tr_cam_mirror_reg(90), 0, &ux, &uy);
		assert(ux == 0 && uy == 0);
	}

	/* 3. The arm controls through the whole chain, at every rotation: the
	 * player's PHYSICAL left arm (the scene's, facing the camera) must read
	 * as LEFT, the right as RIGHT, both as a jump -- whichever rotation the
	 * camera is mounted at, and with the selfie mirror on or off
	 * (TR_CAM_MIRROR, this file is built both ways by runner.sh). The mirror
	 * flips which screen side a limb shows on; MoveNet relabels by what it
	 * sees; pose.c's rule (shoulder x order + TR_CAM_MIRROR) must still land
	 * on the player's own side. */
	for (int rot = 0; rot <= 270; rot += 90) {
		if (rot == 180) {
			continue;
		}
		uint16_t   flip = TR_CAM_MIRROR ? tr_cam_mirror_reg(rot) : 0u;
		int        uh   = TR_CAM_UP_H(rot);
		tr_track_t t;

		tr_pose_t down = as_delivered(player(TR_CAM_UP_W(rot), uh, false, false), rot, flip);

		tr_track_init(&t, (int16_t)uh);
		tr_track_calibrate(&t, tr_pose_box(&down));
		assert(t.calibrated);
		(void)hold(&t, rot, flip, false, false, 3); /* primed, both arms down */

		tr_intent_t in = hold(&t, rot, flip, true, false, 10);

		assert(in.lane_delta == -1 && !in.jump);
		(void)hold(&t, rot, flip, false, false, 3);
		in = hold(&t, rot, flip, false, true, 10);
		assert(in.lane_delta == +1 && !in.jump);
		(void)hold(&t, rot, flip, false, false, 3);
		in = hold(&t, rot, flip, true, true, 10);
		assert(in.lane_delta == 0 && in.jump);
	}

	printf("PASS: tests/host/test_cam_mirror.c\n");
	return 0;
}
