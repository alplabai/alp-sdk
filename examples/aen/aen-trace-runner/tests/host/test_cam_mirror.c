/* tests/host/test_cam_mirror.c -- the selfie mirror (src/vision/cam_rot.h
 * TR_CAM_MIRROR): the sensor bit tr_cam_mirror_reg() names, followed by
 * the software rotation tr_cam_rot_src(), must mirror the UPRIGHT view
 * left/right -- on every pixel, at every rotation -- and the game must read
 * the player's physical left as screen-left and lane-left.
 *
 * The sensor is modelled by what its flip bits do to the raw readout: VFLIP
 * (0x3820 bit 2) reverses the rows, HMIRROR (0x3821 bit 2) the columns. */
#include <assert.h>
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

static tr_pose_t figure(int cx)
{
	tr_pose_t p = { 0 };

	for (int k = 0; k < TR_POSE_KP; k++) {
		p.kp[k] = (tr_kp_t){ (int16_t)cx, 300, 200 };
	}
	p.kp[TR_KP_NOSE] = (tr_kp_t){ (int16_t)cx, 100, 200 };
	p.kp[TR_KP_LSHO] = (tr_kp_t){ (int16_t)(cx + 40),
		                          160,
		                          200 }; /* anatomical left: image right, facing the camera */
	p.kp[TR_KP_RSHO] = (tr_kp_t){ (int16_t)(cx - 40), 160, 200 };
	p.kp[TR_KP_LHIP] = (tr_kp_t){ (int16_t)(cx + 30), 360, 200 };
	p.kp[TR_KP_RHIP] = (tr_kp_t){ (int16_t)(cx - 30), 360, 200 };
	p.kp[TR_KP_LANK] = (tr_kp_t){ (int16_t)(cx + 30), 600, 200 };
	p.kp[TR_KP_RANK] = (tr_kp_t){ (int16_t)(cx - 30), 600, 200 };
	return p;
}

/* The same pose as the mirrored sensor delivers it: every x reflected. */
static tr_pose_t mirrored(tr_pose_t p, int uw)
{
	for (int k = 0; k < TR_POSE_KP; k++) {
		p.kp[k].x = (int16_t)(uw - 1 - p.kp[k].x);
	}
	return p;
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

	/* 3. Game intent in the mirrored view (track.h TR_CAM_MIRROR_X stays 0:
	 * the mirror is already in the pixels). Facing the camera, a player who
	 * steps to their OWN left moves to larger x in the unmirrored upright
	 * frame; the mirrored sensor puts them at screen-left, and the tracker
	 * must say LEFT. The box is anatomy-agnostic (L/R means and the
	 * shoulder width's magnitude), so MoveNet's L/R labels -- which side of the
	 * image they sit on flips with the mirror -- cannot swap the lane. */
	{
		int         uw = TR_CAM_UP_W(90), uh = TR_CAM_UP_H(90);
		tr_track_t  t;
		tr_pose_t   stand = mirrored(figure(uw / 2), uw);
		tr_pose_t   step  = mirrored(figure(uw / 2 + 130), uw); /* the player's own left */
		tr_intent_t in;

		assert(tr_pose_box(&step).x + tr_pose_box(&step).w / 2 < uw / 3);
		tr_track_init(&t, (int16_t)uw, (int16_t)uh);
		tr_track_calibrate(&t, tr_pose_box(&stand), (int16_t)uw);
		in = tr_track_update(&t, tr_pose_box(&step));
		assert(in.lane_delta == -1 && !in.jump && !in.duck);

		/* L/R labels swapped (MoveNet reading the mirrored body): same box. */
		tr_pose_t sw = step;

		for (int k = TR_KP_LEYE; k < TR_POSE_KP; k += 2) {
			tr_kp_t tmp = sw.kp[k];

			sw.kp[k]     = sw.kp[k + 1];
			sw.kp[k + 1] = tmp;
		}
		tr_box_t a = tr_pose_box(&step), b = tr_pose_box(&sw);

		assert(a.x == b.x && a.w == b.w && a.y == b.y && a.h == b.h);

		/* jump unchanged by the mirror: a vertical move only */
		tr_track_resync(&t, 1u);
		tr_track_calibrate(&t, tr_pose_box(&stand), (int16_t)uw);
		(void)tr_track_update(&t, tr_pose_box(&stand)); /* seeds the baseline */
		tr_pose_t up = stand;

		for (int k = 0; k < TR_POSE_KP; k++) {
			up.kp[k].y = (int16_t)(up.kp[k].y - 120);
		}
		(void)tr_track_update(&t, tr_pose_box(&up));
		in = tr_track_update(&t, tr_pose_box(&up)); /* the second frame: TR_TRACK_DEBOUNCE */
		assert(in.jump && in.lane_delta == 0);
	}

	printf("PASS: tests/host/test_cam_mirror.c\n");
	return 0;
}
