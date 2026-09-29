/* src/render/r3d_scene.h -- the Trace Runner 3D scene: a low-poly run down a
 * printed circuit board. Turns the A32's only input, the mailbox snapshot
 * tr_frame_in_t (src/ipc/tr_mbox.h), into a camera + display list for
 * tr_bin_build()/tr_raster_band(), plus the band background (tr_bg_t).
 * Plans: docs/superpowers/plans/2026-09-22-real-3d-renderer.md sections 1
 * and 5 (T5/T6), adapted to the z-banded A32 renderer of
 * 2026-09-22-a32-renderer.md section 7 (so emission order is free).
 *
 * Pure C, float front-end (-ffp-contract=off, tr_sincosf, no libm): the
 * same bits on the host and the A32, so a host golden frame means
 * something on glass. Banner/HUD are NOT drawn here.
 */
#ifndef TR_R3D_SCENE_H
#define TR_R3D_SCENE_H

#include <stdint.h>

#include "../ipc/tr_mbox.h"
#include "r3d.h"
#include "r3d_rig.h"

#define TR_PARTICLES 32
#define TR_SCARF_PTS 7 /* >= meshes.h TR_SCARF_MAX + 1 */
#define TR_TILE_LEN  384
#define TR_TILES     66 /* from z = -2 tiles (under the camera) out past r3d_scene.c GROUND_END 23,800 */

/* ponytail: calibration knobs, tune on glass. World units (lane 240,
 * runner depth TR_PROJ_Z_RUNNER 256), degrees, px. */
/* P3c ("the character should come back a bit"): eye 300 -> 400 up, 400 ->
 * 360 back, pitch 19 -> 18, f 560 -> 600 -- the horizon stays (445 px, was
 * 447: the HUD band untouched), the runner's feet drop 821 -> 987 px (13 %
 * of the screen lower), its height stays 243 px, and more road shows ahead. */
#define TR_CAM_EYE_H     400.0f
#define TR_CAM_BACK      360.0f
#ifndef TR_CAM_PITCH_DEG
/* fix round 9: 18.0 -> 23.0 -- the collision ground_y (tr_runner_ground_y,
 * unmoved, r3d_scene.h/src/game/state.h) is the AVERAGE foot position, not
 * the rendered sprite's own worst-case extent: the running animation's
 * planted foot reaches measurably lower at some phases of the stride than
 * the flat collision line implies. At the un-retuned 18 deg the rendered
 * boot touched TR_VIEW_H's own bottom row (0-3 px margin, real-pixel finding
 * off the composited full-screen render, not the collision-line proxy --
 * see test_r3d_scene.c's own framing assert comment for why the proxy alone
 * under-counts this).
 *
 * The direction that actually helps is UP, not down: pitching down MORE
 * (bigger TR_CAM_PITCH_DEG) reveals more ground plane between the eye and
 * the runner's fixed follow distance (RUNNER_Z - TR_CAM_BACK, r3d_scene.c),
 * lifting the boot up-frame; pitching down LESS (14-16 deg) does the
 * opposite and was measured WORSE (16-40 px) on the real composited render,
 * the reverse of an earlier, since-corrected reading that had accidentally
 * measured a stale/mismatched render (a shared throwaway output path
 * overwritten by an unrelated sweep run). 23 deg measures ~88 px of real
 * boot-to-edge road on the composited render (tick 1234, every zone),
 * inside the 60-90 px ask; every 1 deg step from 18 to 23 was checked
 * against tests/host/test_r3d_ent_lead.c's MEMORY CANYON far-road-
 * visibility assert (a genuine, non-monotonic single-row gap tripped a few
 * individual degree values in the 14-17 range -- 23 is clean). Camera-only,
 * per the maintainer's own ruling -- the runner's world position (ground_y,
 * scroll speed) is untouched. */
#define TR_CAM_PITCH_DEG 23.0f /* positive looks down (tr_cam_build) */
#endif
/* Half/half layout: the focal length scales with the viewport height, so
 * the vertical framing (horizon, runner feet, road below them) keeps the
 * proportions every knob above was tuned at (600 px over the 853-row view
 * of fix round 9): at TR_VIEW_H 640 that is 450 px -- the same picture 3/4
 * the size, with a wider horizontal view. */
#define TR_CAM_F_PX      (600.0f * (float)TR_VIEW_H / (float)TR_VIEW_TUNED_H)
#define TR_CAM_BANK_DEG  5.0f
/* The skyline (far layer) stands this far ahead of the eye, riding with it;
 * the board runs out to its foot (r3d_scene.c GROUND_END). */
#define TR_SCENE_SKY_DZ 24000.0f

/* tr_r3d_tex[] / tr_r3d_tex_fog[] slots the scene binds: the ground of the
 * zone on the camera's side of the gate, and of the zone beyond it (P15; the
 * same textures when no gate is on the track). The _FAR pair is index-only
 * (tr_r3d_tex[] NULL, r3d.h). Bound by tr_scene_init() and by every build
 * part 0 / 1. */
#define TR_SCENE_TEX_LANE      0
#define TR_SCENE_TEX_BOARD     1
#define TR_SCENE_TEX_LANE_FAR  2
#define TR_SCENE_TEX_BOARD_FAR 3
/* The far road (slots 4-7; index-only): the lane and board textures of the
 * zone on the camera's side of the gate (4, 5) and beyond it (6, 7), averaged
 * down each column -- the texture as it minifies with depth, detail along
 * the road merged, the lines along it kept -- drawn from the end of the near
 * ground to the skyline's foot. 6 and 7 share 4's and 5's index buffers (the
 * other half of their rows). */
#define TR_SCENE_TEX_LANE_AVG      4
#define TR_SCENE_TEX_BOARD_AVG     5
#define TR_SCENE_TEX_LANE_AVG_FAR  6
#define TR_SCENE_TEX_BOARD_AVG_FAR 7

typedef struct {
	tr_v3_t pos, vel;
	float   life; /* 40 Hz frames left at the game pace (fractional); <= 0 = free */
	uint8_t col;  /* tr_r3d_palette index */
	uint8_t spark; /* crash spark: small, culled nearer than the runner */
} tr_particle_t;

typedef struct {
	tr_particle_t p[TR_PARTICLES];
	uint32_t      rng;
	uint32_t      prev_score, prev_flags;
	uint8_t       prev_lane; /* last frame's in->lane: the slide's target before this frame (P3d) */
	float         cam_x, runner_x; /* eased toward the lane centre */
	float         cam_roll;        /* bank, radians */
	float         cam_bob;         /* run bob, world units */
	float         lift;            /* runner jump height, world units */
	float         dip;             /* landing dip, world units (<= 0) */
	float         dip_t;           /* landing dip clock, 40 Hz frames left */
	uint8_t       quality; /* TR_LOD_* bits; 0 (tr_scene_init) = full detail, the golden image */
	/* The runner's pose this frame (r3d_rig.h channels) and the blend
	 * weights of the jump / duck / crash poses over the run cycle and of the
	 * landing squash, each easing 0 <-> 1 over a few frames. */
	float         ch[TR_ANIM_CH];
	float         w_jump, w_duck, w_crash, w_land;
	float         jump_ch[TR_ANIM_CH]; /* the jump's pose where the air time left it */
	float         squash;              /* runner scale_y: squash at contact, stretch in flight */
	float         roll;                /* duck: forward-roll angle, radians (0 upright) */
	float         crash_roll;          /* the roll angle a crash caught, -pi..pi (0: none) */
	/* P16 character. chr: the character drawn (the packet's, validated);
	 * t40: 40 Hz frames since init, wrapped every 256 s (the blink and
 * flutter clock); face: this frame's
	 * eyes; yaw: extra instance yaw (a spin, facing the camera when idle);
	 * w_idle: the idle stance's blend; w_react: the reaction layer's
	 * weight this frame (0: none). */
	uint8_t       chr;
	float         t40;
	tr_face_t     face;
	float         yaw, w_idle, w_react;
	/* The scarf (P16): a verlet chain of scarf_n + 1 world points from the
	 * back of the neck (sc[0] = the anchor), stepped at 120 Hz over each
	 * frame's time; sc_prev the previous positions, sc_anchor last
	 * frame's anchor (the sub-steps interpolate it). scarf_n 0: not laid
	 * out yet (the first frame, a character change). */
	uint8_t       scarf_n;
	float         sc[TR_SCARF_PTS][3], sc_prev[TR_SCARF_PTS][3], sc_anchor[3];
} tr_scene_t;

/* tr_scene_t.quality: cheaper frames on demand (bench A/B, the A32 renderer
 * copies it from a mailbox-page word each frame). Any bit set changes the
 * image; no golden covers it. */
#define TR_LOD_NO_BACK_RANK 0x01u /* drop the half-hidden back rank of wall caps */
#define TR_LOD_NEAR         0x02u /* low-poly meshes from TR_LOD_NEAR_Z instead of 1800 */
#define TR_LOD_STILL        0x04u /* no living-board geometry (P12 LEDs, fans) */
#ifndef TR_LOD_NEAR_Z
#define TR_LOD_NEAR_Z 900.0f
#endif

void tr_scene_init(tr_scene_t *s);

/* Once per frame, before tr_scene_build(): camera easing, lane-change bank,
 * run bob, jump lift, landing dip, pickup particle burst (a score step of
 * >= 10 is a pickup, see step.c), crash spark burst (the first
 * TR_FLAG_CRASH frame) and particle motion (slowed with the crash's
 * slow motion). Everything else in a crash -- knock-back, tumble, shake,
 * flash -- is a pure function of in->crash_tick, done in the build. Every
 * per-frame rate here is per 40 Hz frame, integrated over in->hz's frame
 * time (P3d): a 30 Hz stream and a 40 Hz one look the same in real time. */
void tr_scene_step(tr_scene_t *s, const tr_frame_in_t *in);

/* Builds the camera and the frame's display list (dl->n and tr_dl_dropped
 * are reset here): scrolling ground, component walls, every live entity of
 * in->ents[], the runner, particles, skyline and sun. */
void tr_scene_build(const tr_scene_t *s, const tr_frame_in_t *in, tr_cam_t *cam, tr_dl_t *dl);

/* The same build in two halves for two cores: part 1 = the near (textured)
 * ground + walls (and their P12 deco) of tiles k < TR_SCENE_SPLIT_TILE,
 * part 2 = the rest (far tiles, entities, runner, particles, skyline) -- part 1's DL followed by part 2's is
 * exactly tr_scene_build()'s DL (part 0 = the whole build). Each resets
 * only its own dl->n; the caller zeroes tr_dl_dropped (shared) once. Both
 * write *cam identically. Reentrant: the two parts may run concurrently. */
/* 20: the golden frame splits 862 + 837 tris (14 gave 650 + 1049 once the
 * runner grew to ~700 tris a pose, P3). */
#ifndef TR_SCENE_SPLIT_TILE
#define TR_SCENE_SPLIT_TILE 20
#endif
void tr_scene_build_part(const tr_scene_t *s, const tr_frame_in_t *in, tr_cam_t *cam, tr_dl_t *dl, int part);

/* Band background for `cam`: the dithered sky of the frame's zone (on the
 * board: indigo -> magenta -> the horizon glow at the camera's horizon row),
 * stars, and the halo around the projected sun or moon; glow (= fog colour)
 * below the horizon. Over a gate's pass every one of them blends from the
 * outgoing zone's to the incoming one's (tr_scene_zone()). */
void tr_scene_bg(const tr_frame_in_t *in, const tr_cam_t *cam, tr_bg_t *bg);

/* World zones (P15, src/game/zone.h). From the packet alone: *near = the
 * zone on the camera's side of the gate, *far = beyond it (both the
 * packet's zone, and *gate_z = -1e30, with no gate or no TR_FLAG_ZONE: an
 * old HE's frames are the board), *gate_z = the gate's world depth, moved
 * on by the sub-tick phase like an entity. Returns the look's blend near ->
 * far, 0..1: smooth over the gate's pass from 900 world units before the
 * runner (~0.5 s at the play pace) to 900 after -- the sky stops, glow,
 * fog, sun / moon and skyline all follow it. */
float tr_scene_zone(const tr_frame_in_t *in, uint8_t *near, uint8_t *far, float *gate_z);

/* World depth the build draws in->ents[i] at: its model y moved on by the
 * sub-tick phase (TR_FLAG_PHASE), and held touching the runner's front
 * instead of inside it for the obstacle a TR_FLAG_CRASH frame names in
 * crash_ent, and for any obstacle in the runner's lane, not yet at its
 * line, that the current airborne/ducking flags do not clear. */
float tr_scene_ent_z(const tr_frame_in_t *in, int i);

/* Fog amount (0..1, toward the horizon glow) of an entity at world depth z:
 * 1 at its spawn depth, a fade-in over its first steps, then a light haze
 * rising with depth. fk scales the haze only (emissive arcs and sparks burn
 * through it: fk < 1); the fade-in is whole for every part. */
float tr_scene_ent_fog(float z, float fk);

/* Far LOD size of an entity of `kind` (tr_pkt_ent_t.kind) at world depth
 * z: 1 nearer than the LOD depth, growing with depth to its kind's far size
 * so a far part is recognisable, grown in from 0 at spawn with the fade-in;
 * k / z still falls with depth (the part only ever grows on screen as it
 * comes in). */
float tr_scene_ent_scale(float z, uint8_t kind);

/* Half size of particle `p`'s quad as the build draws it; 0 = not drawn
 * (dead, or a crash spark nearer than the runner). */
float tr_scene_particle_r(const tr_particle_t *p);

/* The runner's local front (largest skinned z, before the instance) in any
 * pose or blend: an obstacle held CONTACT_DZ 100 in front minus its half
 * depth (24, the resistor) touches it without going inside. */
/* fix round 8, REVERTED to 76.0 (maintainer ruling): scroll speed and
 * TR_ANIM_GROUND_V went back to their original, viewport-independent
 * values (tools/genmesh.py), so the character's stride reach is back to
 * what this constant was always tuned for -- no bump needed. */
#define TR_RUNNER_FRONT_Z 76.0f

/* World units hold_front() pulled the drawn runner's pos.z back this frame
 * (0 if it didn't clamp; not roll_about_middle()'s own, expected clamp for
 * a rolled/curled pose -- see r3d_scene.c): test instrumentation, reset at
 * the top of every tr_scene_runner() call. A pose test can catch a
 * front-clamp regression this way that a post-hoc front measurement can't,
 * since the clamp always leaves the measured front inside bound by
 * construction. */
extern float tr_front_pullback;

/* The far parts (past ENT_FAR_Z, r3d_scene.c ent_quad()): each mesh as a
 * few camera-facing quads, x0..x1 across, y0..y1 up, in palette colour col
 * -- the arch's posts and lintel (tr_mesh_arch), the resistor's body and
 * legs (tr_mesh_resistor_lo), a live wire post's stem and head
 * (tr_mesh_post_low_lo / _high_lo). test_r3d_scene.c checks every quad
 * against its mesh's triangles of that colour. */
typedef struct {
	float   x0, y0, x1, y1;
	uint8_t col;
} tr_far_quad_t;
extern const tr_far_quad_t tr_far_arch[3], tr_far_resistor[3], tr_far_post_low[2], tr_far_post_high[2];

/* Height of the side hop on a lane change, world units (ponytail: tune on
 * glass). */
#define TR_LANE_HOP 14.0f

/* The runner as tr_scene_build() draws it: every part of character s->chr
 * (body, head, limbs, the eyes shaped by s->face) posed by s->ch and
 * skinned into xyz / vn, and the one instance that places them (a crash
 * moves the skinned vertices so the rearmost sits at local z 0, the tumble
 * pivot). board_lift: how far on_board() raised it to keep every vertex
 * above the board (0 when the pose already clears it). inst.mesh points
 * into this struct: set it per piece after any copy. */
typedef struct {
	float     xyz[TR_RIG_DRAWN][TR_RIG_MAX_V * 3];
	int8_t    vn[TR_RIG_DRAWN][TR_RIG_MAX_V * 3];
	tr_mesh_t mesh[TR_RIG_DRAWN];
	tr_inst_t inst;
	float     board_lift;
	/* The scarf (P16) as drawn: a strip in WORLD space (sinst is the
	 * identity instance), both windings -- the rasterizer keeps the side
	 * facing the camera. scarf[k].nv 0: none. */
	float     sxyz[TR_SCARF_PTS * 2 * 3];
	int8_t    svn[2][TR_SCARF_PTS * 2 * 3];
	uint8_t   stri[2][(TR_SCARF_PTS - 1) * 6];
	int8_t    sn[2][(TR_SCARF_PTS - 1) * 6];
	uint8_t   scol[(TR_SCARF_PTS - 1) * 2];
	tr_mesh_t scarf[2];
	tr_inst_t sinst;
} tr_runner_draw_t;
/* tr_scene_build_part() keeps one on its stack (each A32 core builds
 * concurrently; a static one would race after a core-1 timeout, when core
 * 0 builds the whole frame while core 1 may still finish its part). Measured
 * -fstack-usage, A32 -O2: P16 build_part 16,832 B (~20 KB deepest chain);
 * P16b (5 parts + eyes): build_part 24,560 B, deepest chain (+ tr_r3d_emit_mesh
 * 8,144 B) ~33 KB -- inside
 * the 64 KiB per-core stack (a32/renderer/start.S). Grow this and
 * re-measure; past ~40 KB deepest, make it a static per-core buffer. */
_Static_assert(sizeof(tr_runner_draw_t) <= 24u * 1024u, "tr_runner_draw_t: the A32 build_part stack budget (see above)");

void tr_scene_runner(const tr_scene_t *s, const tr_frame_in_t *in, tr_runner_draw_t *r);

/* World units the ground has scrolled at `tick` + phase/65536 ticks. */
uint32_t tr_scene_scroll(uint32_t tick, uint16_t phase);

/* The crash's flash on the band background: call after tr_scene_bg().
 * Blends all three colours toward red (blue-white for TR_CRASH_KIND_WIRE),
 * fading out over the first ~0.5 s of a TR_FLAG_CRASH sequence; leaves `bg`
 * untouched on any other frame. */
void tr_scene_bg_flash(const tr_frame_in_t *in, tr_bg_t *bg);

/* The camera shake this frame, px, as a screen-space jolt of the
 * projection centre (tr_scene_build adds it to cam->cx / cy): with
 * TR_FLAG_SHAKE the packet's `shake` (255 = TR_SCENE_SHAKE_PX), the booth
 * HE's; else, on a crash frame (an old HE), the renderer's own 14 px fade
 * over ~0.5 s of crash_tick; else 0. */
#define TR_SCENE_SHAKE_PX 18.0f
void tr_scene_shake(const tr_frame_in_t *in, float *dx, float *dy);

/* Shoulder parts are culled when a sphere of TR_WALL_R around (x, 100, z)
 * projects wholly off screen. It bounds every wall mesh (tr_wall_lod) at
 * any yaw and at the tallest y scale a part is drawn with, TR_WALL_SY_MAX
 * (the back-rank cap, 451 units: 356 from the centre), with 10 % to spare
 * because the cull's r = R f / vz understates an off-axis sphere's
 * projection (~9 % at 45 degrees); test_r3d_scene checks the bound and
 * that the cull never changes a pixel. tr_scene_wall_r is the radius the
 * build uses: a test hook (a huge value turns the cull off, the P12 fan and
 * LED culls with it -- their radii scale from it). */
#define TR_WALL_R      400.0f
#define TR_WALL_SY_MAX 2.15f
extern float tr_scene_wall_r;

/* Past this world depth (the part's) a shoulder part is drawn as a
 * billboard: one camera-facing quad of its size and mean colour, fogged (2
 * triangles, not its 6-24). tr_scene_wall_bb_z is what the build uses: a
 * test hook (test_scene_load.c measures the step the switch makes). */
#define TR_WALL_BB_Z 9000.0f
extern float tr_scene_wall_bb_z;

/* Live wires (P4b, entity kind 3). The seed a frame draws ents[i]'s arc and
 * sparks from: tick, sub-tick phase, crash tick and slot, hashed -- the arc
 * is re-randomised every frame yet a frame rebuilt is the same frame. */
uint32_t tr_scene_wire_seed(const tr_frame_in_t *in, int i);

/* The arc's jagged polyline from a to b, out[0..n] (out[0] == a, out[n] ==
 * b): the straight line bowed down by `bow` mid-way, every inner point
 * jittered from `seed` by up to amp (y), 0.4 amp (x), 0.3 amp (z), the
 * jitter tapering toward the ends. */
void tr_scene_arc(uint32_t seed, tr_v3_t a, tr_v3_t b, float bow, float amp, int n, tr_v3_t *out);

/* ------------------------------------------------ living circuit board (P12)
 * Everything is a pure function of tick + sub-tick phase (and the tile hash):
 * a frame rebuilt is the same frame. */

/* Shoulder deco of world tile `tile`, side -1 / +1, near edge at z0, at
 * quality q (the walls' tile hash picks it): a spinning fan on some QFPs and
 * BGAs drawn full detail (near only, the walls' LOD threshold incl.
 * TR_LOD_NEAR; fan.y == 0: none), and SMD LEDs at the lane edge -- n_led 0,
 * 1 (blinking) or 3 (chasing), led_on bit k = LED k lit now, led_lo: past the
 * LOD threshold (body top only). Animated from tick + sub-tick phase. */
typedef struct {
	uint32_t h;        /* the tile hash walls() picks the parts from: h & 7 = 3 QFP, 4 BGA */
	tr_v3_t fan;       /* rotor + frame centre on the package top */
	float   fan_yaw;   /* rotor angle now, radians */
	float   frame_yaw; /* the package's twist */
	tr_v3_t led;       /* first LED, on the board; a row goes on outward (away from the lanes) */
	uint8_t n_led, led_col, led_on, led_lo;
} tr_deco_t;
void tr_scene_deco(uint32_t tile, int side, float z0, uint8_t q, const tr_frame_in_t *in, tr_deco_t *d);

#endif /* TR_R3D_SCENE_H */
