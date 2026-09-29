/* tests/host/test_movenet.c -- src/vision/movenet.c (the integer CPU tail)
 * on two real frames of cut-model output (tests/host/data/movenet_*.bin,
 * tools/npu_body_proto.py --vectors): it must match MoveNet's own tail run
 * in float to within rounding, and the FULL published model's confident
 * keypoints to within the cell-level ties its quantised tail breaks
 * differently. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/vision/movenet.h"
#include "data/movenet_ref.h"

#define MAP_BYTES (TR_MN_CELLS * (1 + 17 + 34 + 34))

static int8_t g_maps[MAP_BYTES];

static int manhattan(const tr_kp_t *a, const int16_t b[3])
{
	return abs(a->x - b[0]) + abs(a->y - b[1]);
}

static void check(const char *name, const int16_t flt[17][3], const int16_t ref[17][3])
{
	char path[256];

	snprintf(path, sizeof(path), "tests/host/data/movenet_%s.bin", name);
	FILE *f = fopen(path, "rb");
	assert(f != NULL);
	assert(fread(g_maps, 1, MAP_BYTES, f) == MAP_BYTES);
	fclose(f);

	tr_movenet_out_t o = {
		.centre  = g_maps,
		.heat    = g_maps + TR_MN_CELLS,
		.offset  = g_maps + TR_MN_CELLS * 18,
		.regress = g_maps + TR_MN_CELLS * 52,
	};
	tr_pose_t p;

	tr_movenet_decode(&o, 640, 400, &p);

	for (int k = 0; k < TR_POSE_KP; k++) {
		printf("%s kp%2d c=(%4d,%4d,%3d) float=(%4d,%4d,%3d) ref=(%4d,%4d,%3d)\n", name, k, p.kp[k].x, p.kp[k].y,
		       p.kp[k].score, flt[k][0], flt[k][1], flt[k][2], ref[k][0], ref[k][1], ref[k][2]);
		assert(manhattan(&p.kp[k], flt[k]) <= 2);    /* the same cell, same offset, rounding only */
		assert(abs(p.kp[k].score - flt[k][2]) <= 1);
		/* Against the published model, same pixels: its tail quantises the
		 * distance map (0.24-cell steps) and so breaks near-ties a cell or
		 * two away. Every keypoint it is confident in lands within three
		 * 13.3 px cells (|dx|+|dy|), the torso -- pose.c's box -- within two. */
		if (ref[k][2] >= TR_POSE_KP_MIN) {
			bool torso = k == TR_KP_LSHO || k == TR_KP_RSHO || k == TR_KP_LHIP || k == TR_KP_RHIP;

			assert(manhattan(&p.kp[k], ref[k]) <= (torso ? 27 : 40));
		}
	}
}

int main(void)
{
	check("stand", movenet_float_stand, movenet_ref_stand);
	check("crouch", movenet_float_crouch, movenet_ref_crouch);
	return 0;
}
