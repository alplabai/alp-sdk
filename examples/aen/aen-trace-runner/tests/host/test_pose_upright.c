/* tests/host/test_pose_upright.c -- the half/half layout's pose path in
 * UPRIGHT coordinates (src/vision/cam_rot.h: the sensor on its side, a
 * 400x640 portrait frame):
 *   1. tr_movenet_decode() back-maps the portrait letterbox (36 pad input
 *      columns each side) -- the content corners of the 192 square land on
 *      the upright frame's corners, the padding off the frame;
 *   2. pose -> box -> tracker intents from a synthetic upright pose: lane
 *      from torso x across 400 px, jump from the head/shoulders rising,
 *      duck from the box shrinking, over a 640-px-tall frame. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/vision/movenet.h"

#define UW TR_CAM_UP_W(90) /* 400 */
#define UH TR_CAM_UP_H(90) /* 640 */

static int8_t centre[TR_MN_CELLS], heat[TR_MN_CELLS * TR_POSE_KP], offs[TR_MN_CELLS * 34], regr[TR_MN_CELLS * 34];

/* Every keypoint's heat (and the person centre) on one cell, zero offsets:
 * the keypoint is that cell's corner, input px (4 * gx, 4 * gy). */
static tr_pose_t decode_at(int gx, int gy)
{
	tr_movenet_out_t o = { centre, heat, offs, regr };
	tr_pose_t        p;
	int              j = gy * TR_MN_GRID + gx;

	memset(centre, -128, sizeof(centre));
	memset(heat, -128, sizeof(heat));
	memset(offs, TR_MN_OFF_ZP, sizeof(offs));
	memset(regr, TR_MN_REG_ZP, sizeof(regr));
	centre[j] = 127;
	for (int k = 0; k < TR_POSE_KP; k++) {
		heat[j * TR_POSE_KP + k] = 127;
	}
	tr_movenet_decode(&o, UW, UH, &p);
	return p;
}

/* The letterbox inverse, in float, independent of movenet.c's integers:
 * input px -> upright px, the long side (640) scaled to 192, 36 pad cols. */
static int up_x(int in_x)
{
	return (int)lroundf((float)(in_x - 36) * (float)UH / (float)TR_MN_IN);
}

static int up_y(int in_y)
{
	return (int)lroundf((float)in_y * (float)UH / (float)TR_MN_IN);
}

static tr_pose_t figure(int cx, int top, int feet)
{
	static const struct {
		int8_t  dx;
		uint8_t fy; /* % of height below the head top */
	} body[TR_POSE_KP] = {
		{ 0, 7 },    { 4, 5 },    { -4, 5 },   { 8, 6 },    { -8, 6 },   { 25, 20 },
		{ -25, 20 }, { 30, 35 },  { -30, 35 }, { 30, 48 },  { -30, 48 }, { 15, 52 },
		{ -15, 52 }, { 15, 75 },  { -15, 75 }, { 15, 100 }, { -15, 100 },
	};
	tr_pose_t p;

	for (int k = 0; k < TR_POSE_KP; k++) {
		p.kp[k] = (tr_kp_t){ (int16_t)(cx + body[k].dx), (int16_t)(top + (feet - top) * body[k].fy / 100), 200 };
	}
	return p;
}

int main(void)
{
	/* 1. corners: content spans input columns 36..155 (cells 9..38) */
	{
		static const int cells[4][2] = { { 9, 0 }, { 38, 0 }, { 9, 47 }, { 38, 47 } };

		for (int i = 0; i < 4; i++) {
			tr_pose_t p = decode_at(cells[i][0], cells[i][1]);

			for (int k = 0; k < TR_POSE_KP; k++) {
				int ex = up_x(4 * cells[i][0]), ey = up_y(4 * cells[i][1]);

				assert(p.kp[k].score == 255);
				assert(abs(p.kp[k].x - ex) <= 1 && abs(p.kp[k].y - ey) <= 1);
				assert(p.kp[k].x >= 0 && p.kp[k].x < UW && p.kp[k].y >= 0 && p.kp[k].y < UH);
			}
			printf("decode: cell (%d,%d) -> upright (%d,%d)\n", cells[i][0], cells[i][1], p.kp[0].x, p.kp[0].y);
		}
		assert(decode_at(9, 0).kp[0].x == 0 && decode_at(9, 0).kp[0].y == 0); /* exactly the top-left corner */
		/* the padding columns decode OFF the upright frame (and so are never
		 * drawn, cam_pip.h tr_cam_pip_map_kp) */
		assert(decode_at(0, 20).kp[0].x < 0 && decode_at(47, 20).kp[0].x >= UW);
	}

	/* 2. intents from an upright pose: 400 wide, 640 tall */
	{
		tr_track_t  t;
		tr_intent_t in;
		tr_pose_t   stand = figure(UW / 2, 100, 600);
		tr_box_t    b     = tr_pose_box(&stand);

		assert(b.valid && b.x == UW / 2 - 25 && b.w == 50 && b.y == 200 && b.h == 160); /* the torso box */
		tr_track_init(&t, UW, UH);
		assert(t.lane_edges[0] == UW / 3 && t.lane_edges[1] == 2 * UW / 3);
		tr_track_calibrate(&t, b, UW);
		assert(t.calibrated && t.lane == 1u);

		in = tr_track_update(&t, b); /* standing still: nothing */
		assert(in.source == TR_INPUT_VISION && in.lane_delta == 0 && !in.jump && !in.duck);

		tr_pose_t left = figure(60, 100, 600); /* torso into the left third */

		in = tr_track_update(&t, tr_pose_box(&left));
		assert(in.lane_delta == -1 && t.lane == 0u);
		tr_track_resync(&t, 1u);

		tr_pose_t up = figure(UW / 2, 100 - 120, 600 - 120); /* the whole body 120 px up (0.75 of the torso) */

		(void)tr_track_update(&t, tr_pose_box(&up));
		in = tr_track_update(&t, tr_pose_box(&up)); /* the second frame: TR_TRACK_DEBOUNCE */
		assert(in.jump && !in.duck && in.lane_delta == 0);

		for (int i = 0; i < TR_TRACK_HOLD_MIN + TR_TRACK_LAND_COOLDOWN_FRAMES + 2; i++) {
			(void)tr_track_update(&t, b); /* land */
		}
		tr_pose_t crouch = figure(UW / 2, 100 + 80, 600 + 80); /* the torso 80 px down (0.5 of it) */

		(void)tr_track_update(&t, tr_pose_box(&crouch));
		in = tr_track_update(&t, tr_pose_box(&crouch));
		assert(in.duck && !in.jump);
		printf("intents: lane from torso x over %d px, jump, duck over %d px -- upright\n", UW, UH);
	}
	return 0;
}
