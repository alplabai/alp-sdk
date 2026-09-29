/* tests/host/test_proj.c */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "../../src/game/state.h"
#include "../../src/render/proj.h"

int main(void)
{
	/*
	 * 1. THE test that matters most: at the runner's own depth, the 3D
	 * screen y must land on the exact same row the flat 2D game already
	 * collides against -- tr_runner_ground_y() itself, the REAL function
	 * from state.h, not the literal 1104, so this and that function can
	 * never drift apart silently.
	 */
	assert(tr_proj_screen_y(TR_PROJ_Z_RUNNER) == tr_runner_ground_y(1280));

	/* 2. Scale at the runner's depth is exactly 1.0x. */
	assert(tr_proj_scale_q16(TR_PROJ_Z_RUNNER) == TR_PROJ_SCALE_ONE);

	/* 3. Lane centres at the runner's depth match today's flat lane_x()
	 * band centres exactly: 120, 360, 600 (720 / 3 wide lanes). */
	assert(tr_proj_lane_center_x(0, TR_PROJ_Z_RUNNER) == 120);
	assert(tr_proj_lane_center_x(1, TR_PROJ_Z_RUNNER) == 360);
	assert(tr_proj_lane_center_x(2, TR_PROJ_Z_RUNNER) == 600);

	/*
	 * 4. Monotonic with depth: farther (larger z) means smaller scale and
	 * a smaller screen y, and lane edges converge toward CENTER_X.
	 */
	{
		int32_t z_near = TR_PROJ_Z_RUNNER;
		int32_t z_far  = TR_PROJ_Z_RUNNER * 4;

		assert(tr_proj_scale_q16(z_far) < tr_proj_scale_q16(z_near));
		assert(tr_proj_screen_y(z_far) < tr_proj_screen_y(z_near));

		for (uint8_t edge = 0; edge <= TR_PROJ_LANES; edge++) {
			int32_t dist_near = tr_proj_lane_edge_x(edge, z_near) - TR_PROJ_CENTER_X;
			int32_t dist_far  = tr_proj_lane_edge_x(edge, z_far) - TR_PROJ_CENTER_X;

			if (dist_near < 0) {
				dist_near = -dist_near;
			}
			if (dist_far < 0) {
				dist_far = -dist_far;
			}
			assert(dist_far <= dist_near);
		}
	}

	/*
	 * 5. At Z_FAR, screen y sits within a few px of the horizon (it's
	 * exactly HORIZON_Y + 20 by construction -- see proj.h), and every
	 * lane edge falls in a narrow band around CENTER_X.
	 */
	{
		int32_t sy = tr_proj_screen_y(TR_PROJ_Z_FAR);

		assert(sy >= TR_PROJ_HORIZON_Y && sy <= TR_PROJ_HORIZON_Y + 25);

		for (uint8_t edge = 0; edge <= TR_PROJ_LANES; edge++) {
			int32_t ex   = tr_proj_lane_edge_x(edge, TR_PROJ_Z_FAR);
			int32_t dist = ex - TR_PROJ_CENTER_X;

			if (dist < 0) {
				dist = -dist;
			}
			assert(dist <= 12); /* narrow band: outermost edge sits +-10 px at Z_FAR */
		}
	}

	/*
	 * 6. Round trip: scanline-to-depth of depth-to-screen-y returns the
	 * original depth within stated rounding. Two floor divisions
	 * (screen_y quantizes z to an integer row, scanline_depth then
	 * requantizes that row back to z) lose precision that grows with the
	 * depth itself -- one screen row represents a wider and wider span of
	 * z the farther out it is, which is inherent to K/z, not a bug.
	 * Tolerance 2*(z*z/K) + 8 was checked against every integer z in
	 * [Z_NEAR, Z_FAR] (see this task's report) and holds with margin
	 * everywhere in that range.
	 */
	{
		int32_t sample_z[] = {TR_PROJ_Z_NEAR, 100, TR_PROJ_Z_RUNNER, 1000, 4096, TR_PROJ_Z_FAR};

		for (size_t i = 0; i < sizeof(sample_z) / sizeof(sample_z[0]); i++) {
			int32_t z    = sample_z[i];
			int32_t sy   = tr_proj_screen_y(z);
			int32_t z2   = tr_proj_scanline_depth(sy);
			int32_t diff = z2 - z;
			int32_t tol  = 2 * (z * z / TR_PROJ_K) + 8;

			if (diff < 0) {
				diff = -diff;
			}
			assert(diff <= tol);
		}
	}

	/* 7. Model mapping endpoints: model y=0 gives Z_FAR, model y=1104 (the
	 * runner, tr_runner_ground_y(1280)) gives exactly Z_RUNNER. */
	assert(tr_proj_depth_of_model_y(0) == TR_PROJ_Z_FAR);
	assert(tr_proj_depth_of_model_y(1104) == TR_PROJ_Z_RUNNER);
	assert(1104 == tr_runner_ground_y(1280)); /* the two 1104s above are the same number */

	/*
	 * 8. Near plane: a depth below Z_NEAR is not visible, and every model
	 * y from 0 to 1279 (despawn triggers at 1280, step.c) produces either
	 * a sane on-screen result or a not-visible verdict -- never garbage
	 * from a division by zero or a negative depth.
	 */
	assert(!tr_proj_visible(TR_PROJ_Z_NEAR - 1));
	assert(tr_proj_visible(TR_PROJ_Z_NEAR));
	assert(!tr_proj_visible(-1));
	assert(!tr_proj_visible(0));

	for (int32_t y = 0; y < 1280; y++) {
		int32_t z = tr_proj_depth_of_model_y((int16_t)y);

		/*
		 * Called UNCONDITIONALLY, not just when tr_proj_visible(z) is
		 * true: y climbs past the runner (1104) toward despawn (1280)
		 * and z goes negative well before 1280 (see proj.h's Z_NEAR
		 * comment) -- this is exactly the case a caller that forgot to
		 * check visibility first would hit, and it's what a missing
		 * near-plane clamp turns into a division by zero or a negative
		 * depth. The result is only asserted sane when z is actually
		 * visible; the point for the rest is simply that the call
		 * returns at all instead of trapping.
		 */
		int32_t sy    = tr_proj_screen_y(z);
		int32_t scale = tr_proj_scale_q16(z);

		(void)tr_proj_lane_center_x(1, z);
		(void)tr_proj_lane_edge_x(0, z);

		/*
		 * Unconditional, regardless of tr_proj_visible(z): as long as
		 * the internal depth clamp holds (zz >= TR_PROJ_Z_NEAR > 0),
		 * K/zz can never be negative, so screen y can never fall below
		 * the horizon, and F/zz can never be <= 0. A clamp that quietly
		 * passes a non-positive z straight through to the division --
		 * exactly the near-plane bug this task calls out -- shows up
		 * here as sy dropping below the horizon or scale going
		 * non-positive, for y values well before 1280 where z has gone
		 * negative (see proj.h's Z_NEAR comment).
		 */
		assert(sy >= TR_PROJ_HORIZON_Y);
		assert(scale > 0);

		if (tr_proj_visible(z)) {
			assert(sy <= TR_PROJ_HORIZON_Y + TR_PROJ_K); /* not astronomically off */
		}
	}

	return 0;
}
