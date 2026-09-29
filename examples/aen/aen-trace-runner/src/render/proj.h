/* src/render/proj.h -- perspective projection maths for the 3D track.
 *
 * Pure geometry, no drawing: this module maps the game model's existing
 * `y` (0 = spawn, tr_runner_ground_y() = the runner, see src/game/state.h)
 * onto a screen position and a size, for a camera looking down the track.
 * The game model itself does not change -- see this task's brief -- so
 * every constant here is DERIVED from making the 3D picture agree with
 * where the 2D game already puts things, not picked independently.
 *
 * Pinhole model:
 *   screen_y(z)          = TR_PROJ_HORIZON_Y + TR_PROJ_K / z
 *   screen_x(world_x, z) = TR_PROJ_CENTER_X + world_x * TR_PROJ_F / z
 *   scale(z)             = TR_PROJ_F / z
 *
 * `z` is depth in world units, larger = farther. Deliberately free of any
 * Zephyr/alp-sdk header (see tests/host/runner.sh, which compiles this
 * into every host test).
 */
#ifndef TR_PROJ_H
#define TR_PROJ_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Three anchors at the runner's own depth pin down the whole projection:
 *   1. scale(Z_RUNNER) must be exactly 1.0 -- the sprite size the model
 *      already draws today.
 *   2. screen_y(Z_RUNNER) must equal tr_runner_ground_y(1280) == 1104
 *      exactly, so 3D and 2D agree at the one depth collisions happen.
 *   3. one lane at Z_RUNNER must be exactly 240 px wide -- today's
 *      lane_x() band (720 / TR_LANES), see src/render/render.c.
 * Get any of these wrong and the runner's own sprite -- what the player
 * stares at every tick -- visibly disagrees between the flat collision
 * model and the 3D render.
 */

/*
 * Anchors 1+2: scale(Z_RUNNER) = F / Z_RUNNER must be 1.0, and Z_RUNNER is
 * otherwise a free choice of world units, so the simplest solution is
 * F == Z_RUNNER. 256 is an arbitrary but convenient world-unit scale (a
 * round power of two).
 */
#define TR_PROJ_Z_RUNNER 256
#define TR_PROJ_F         TR_PROJ_Z_RUNNER

#define TR_PROJ_HORIZON_Y 384
#define TR_PROJ_CENTER_X  360

/*
 * Anchor 3: lane width in world units equals lane width in screen px
 * *because* scale is 1.0 at Z_RUNNER (anchor 1) -- world_x and screen_x
 * differ only by the CENTER_X offset at that one depth.
 */
#define TR_PROJ_LANES  3
#define TR_PROJ_LANE_W 240

/*
 * Anchor 2, solved for K: screen_y(Z_RUNNER) == HORIZON_Y + K / Z_RUNNER
 * must equal 1104 (tr_runner_ground_y(1280) -- checked against the real
 * function, not this literal, in tests/host/test_proj.c), so
 * K = (1104 - HORIZON_Y) * Z_RUNNER == 184320.
 */
#define TR_PROJ_K ((1104 - TR_PROJ_HORIZON_Y) * TR_PROJ_Z_RUNNER)

/*
 * Z_FAR: the spawn depth (model y == 0). Chosen so a freshly spawned
 * entity sits ~20 px below the horizon -- visibly "just popped in near
 * the vanishing point", not a zero-height sliver sitting exactly on it:
 *   screen_y(Z_FAR) == HORIZON_Y + 20   <=>   K / Z_FAR == 20
 *   Z_FAR == K / 20 == 9216
 * giving screen_y(Z_FAR) == 404 exactly and scale(Z_FAR) == F/Z_FAR, a
 * small fraction of 1x as a far-away entity should be.
 */
#define TR_PROJ_Z_FAR (TR_PROJ_K / 20)

/*
 * Z_NEAR: the near-clip plane. Model y runs 0..1279 (despawn triggers at
 * 1280, see step.c), and z(y) is LINEAR (see tr_proj_depth_of_model_y()),
 * so it keeps falling past Z_RUNNER as an entity travels beyond the
 * runner toward despawn, crosses zero, and goes negative -- K/z and
 * world_x*F/z would then divide by zero or a negative number and hand
 * back garbage coordinates. TR_PROJ_Z_NEAR=32 stops well short of that
 * (z reaches exactly 0 past model y ~= 1135.6): every public function
 * below clamps its depth to at least TR_PROJ_Z_NEAR before dividing, and
 * tr_proj_visible() reports false for anything nearer. model y == 1132 is
 * where a scrolling entity first crosses it (z drops to 29, the first
 * integer y with z <= 32); verified by test 8's sweep of every model y in
 * [0, 1279].
 */
#define TR_PROJ_Z_NEAR 32

#define TR_PROJ_SCALE_ONE 65536 /* Q16.16 fixed point: 65536 == 1.0x */

/*
 * Model y (0 = spawn, increasing toward and past the runner -- see
 * state.h) to depth z, world units, LARGER z == farther. Linear in z, not
 * in screen space: constant world-space speed is what makes an
 * approaching entity visually accelerate on screen (K/z is convex), which
 * is the whole point of the 3D switch -- see this function's definition
 * for the derivation and why that must not be "corrected" away.
 */
int32_t tr_proj_depth_of_model_y(int16_t model_y);

/* Screen y for depth z. Clamps z to >= TR_PROJ_Z_NEAR first, so this never
 * divides by a non-positive depth even if called without checking
 * tr_proj_visible() first. */
int32_t tr_proj_screen_y(int32_t z);

/* Sprite scale at depth z, Q16.16 (TR_PROJ_SCALE_ONE == 1.0x). Same
 * non-positive-depth clamp as tr_proj_screen_y(). */
int32_t tr_proj_scale_q16(int32_t z);

/* Screen x of lane `lane`'s centre line (0 .. TR_PROJ_LANES-1) at depth z. */
int32_t tr_proj_lane_center_x(uint8_t lane, int32_t z);

/*
 * Screen x of lane boundary `edge` (0 .. TR_PROJ_LANES, i.e. TR_PROJ_LANES+1
 * edges: the two outer track edges plus the TR_PROJ_LANES-1 lines between
 * lanes) at depth z -- what a renderer draws the converging lane lines
 * from, one call per edge per scanline.
 */
int32_t tr_proj_lane_edge_x(uint8_t edge, int32_t z);

/*
 * Inverse of tr_proj_screen_y(): the depth whose ground plane projects to
 * screen row `screen_y`. Valid only for screen_y strictly below the
 * horizon (screen_y > TR_PROJ_HORIZON_Y) -- a ground renderer calls this
 * once per scanline from just below the horizon down to the bottom of the
 * panel to know what depth (and hence what scale) to sample the ground
 * texture at for that row. At or above the horizon there is no finite
 * depth (the horizon *is* z == infinity); this clamps rather than divide
 * by <= 0, returning TR_PROJ_K (a very large, "may as well be infinite"
 * depth) for those rows instead of crashing.
 */
int32_t tr_proj_scanline_depth(int32_t screen_y);

/* True if z is between the near and far planes inclusive: TR_PROJ_Z_NEAR
 * <= z <= TR_PROJ_Z_FAR. A renderer skips drawing anything this rejects. */
bool tr_proj_visible(int32_t z);

#endif /* TR_PROJ_H */
