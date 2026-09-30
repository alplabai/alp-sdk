/* src/render/proj.c -- see proj.h for the derivation of every constant.
 * Deliberately free of Zephyr and alp-sdk headers -- tests/host/runner.sh
 * compiles this file into every host test. Not wired into render.c yet;
 * that is a later task, gated on a hardware measurement still in
 * progress. */
#include "proj.h"

/*
 * Every public function that divides by a depth runs it through this
 * first, so none of them can divide by zero or a negative number even if
 * a caller skips tr_proj_visible(). Below TR_PROJ_Z_NEAR the true K/z or
 * world_x*F/z result would be garbage (or undefined for z <= 0) anyway --
 * clamping to the near plane instead pins it to whatever tr_proj_screen_y/
 * tr_proj_scale_q16 report AT the near plane, which is at least a sane,
 * bounded number a caller that forgot to check visibility won't crash on.
 */
static int32_t clamp_z(int32_t z)
{
	return (z < TR_PROJ_Z_NEAR) ? TR_PROJ_Z_NEAR : z;
}

int32_t tr_proj_depth_of_model_y(int16_t model_y)
{
	/*
	 * z(y) = Z_FAR - (Z_FAR - Z_RUNNER) * y / 1104
	 *
	 * Linear in z (not in screen space): the model already scrolls
	 * entities at a constant TR_SCROLL_PX per tick (step.c), i.e.
	 * constant speed in world space, so this is the direct reading of
	 * that motion as depth. Constant world-space speed is exactly what
	 * makes screen_y's K/z convex -- an approaching entity's screen
	 * position and size change slowly at first and fast right before it
	 * reaches the runner. That acceleration is the whole visual point of
	 * going 3D (see proj.h); mapping y to z on some curve designed to
	 * make screen motion linear instead would erase it.
	 *
	 * y == 1104 (the runner) must give exactly Z_RUNNER and y == 0 must
	 * give exactly Z_FAR -- both fall out of the formula with no
	 * rounding (1104 divides the (Z_FAR - Z_RUNNER) * y product exactly
	 * at those two endpoints because y itself is 0 or 1104).
	 *
	 * 64-bit intermediate: (Z_FAR - Z_RUNNER) * model_y stays inside
	 * int32 for every model_y this game ever produces (max 1279, giving
	 * ~11.5M), but model_y is a plain int16_t with no caller-side range
	 * check, and TR_PROJ_Z_FAR is a derived constant that would grow if
	 * TR_PROJ_K or the horizon offset ever changed -- widening here once
	 * means a future constant change can't quietly reintroduce overflow.
	 */
	int64_t diff = (int64_t)TR_PROJ_Z_FAR - (int64_t)TR_PROJ_Z_RUNNER;
	int64_t z    = (int64_t)TR_PROJ_Z_FAR - diff * (int64_t)model_y / 1104;

	return (int32_t)z;
}

int32_t tr_proj_screen_y(int32_t z)
{
	int64_t zz = clamp_z(z);

	/* K/z stays well inside int32 for any zz >= TR_PROJ_Z_NEAR (32) with
	 * K == 184320 (max result 5760), but the division is written on a
	 * 64-bit intermediate so this can't start overflowing silently if K
	 * or Z_NEAR are ever retuned. */
	return TR_PROJ_HORIZON_Y + (int32_t)((int64_t)TR_PROJ_K / zz);
}

int32_t tr_proj_scale_q16(int32_t z)
{
	int64_t zz = clamp_z(z);

	/* F << 16 == 256 * 65536 == 16,777,216 -- fits int32 on its own, but
	 * the multiply is done in 64 bits before the divide so this stays
	 * correct if F ever grows (a wider FOV, say) past the point where
	 * F << 16 would overflow int32. */
	return (int32_t)(((int64_t)TR_PROJ_F << 16) / zz);
}

/* World x of lane `lane`'s centre, lanes numbered 0 .. TR_PROJ_LANES-1
 * left to right, symmetric about world x == 0 (which projects to
 * TR_PROJ_CENTER_X at every depth). Lane 1 of 3 sits on 0; lane 0 and
 * lane 2 sit one lane width to either side -- this is the world-space
 * mirror of render.c's lane_x(), which centres the same bands in screen
 * pixels at the flat 2D scale. */
static int32_t lane_center_world_x(uint8_t lane)
{
	return (int32_t)lane * TR_PROJ_LANE_W - (TR_PROJ_LANES - 1) * TR_PROJ_LANE_W / 2;
}

/* World x of lane boundary `edge`, edges numbered 0 .. TR_PROJ_LANES left
 * to right (TR_PROJ_LANES + 1 of them: the two outer track edges plus the
 * TR_PROJ_LANES - 1 lines between lanes). Edge k sits half a lane to the
 * left of centre-line k, i.e. LANE_W/2 less than lane_center_world_x(k). */
static int32_t lane_edge_world_x(uint8_t edge)
{
	return (int32_t)edge * TR_PROJ_LANE_W - TR_PROJ_LANES * TR_PROJ_LANE_W / 2;
}

static int32_t screen_x_of_world(int32_t world_x, int32_t z)
{
	int64_t zz = clamp_z(z);

	/* world_x * F: world_x tops out at +-360 (the outermost lane edge)
	 * and F == 256, so the product never exceeds ~92K -- nowhere near
	 * int32 overflow with today's constants. Same reasoning as
	 * tr_proj_screen_y()/tr_proj_scale_q16(): the multiply-then-divide is
	 * done in 64 bits so a future wider track or bigger F can't silently
	 * start overflowing this. */
	int64_t num = (int64_t)world_x * (int64_t)TR_PROJ_F;

	return TR_PROJ_CENTER_X + (int32_t)(num / zz);
}

int32_t tr_proj_lane_center_x(uint8_t lane, int32_t z)
{
	return screen_x_of_world(lane_center_world_x(lane), z);
}

int32_t tr_proj_lane_edge_x(uint8_t edge, int32_t z)
{
	return screen_x_of_world(lane_edge_world_x(edge), z);
}

int32_t tr_proj_scanline_depth(int32_t screen_y)
{
	/*
	 * Inverse of tr_proj_screen_y(): z = K / (screen_y - HORIZON_Y).
	 * Only defined below the horizon (screen_y > HORIZON_Y); at or above
	 * it the true depth is infinite (the horizon line itself). Clamp the
	 * denominator to >= 1 rather than divide by <= 0 -- this hands back
	 * TR_PROJ_K (184320, itself well past TR_PROJ_Z_FAR == 9216) for any
	 * at-or-above-horizon row, which a caller can treat as "may as well
	 * be infinitely far" the same way tr_proj_visible() would reject it.
	 */
	int64_t denom = (int64_t)screen_y - TR_PROJ_HORIZON_Y;

	if (denom < 1) {
		denom = 1;
	}
	return (int32_t)((int64_t)TR_PROJ_K / denom);
}

bool tr_proj_visible(int32_t z)
{
	return z >= TR_PROJ_Z_NEAR && z <= TR_PROJ_Z_FAR;
}
