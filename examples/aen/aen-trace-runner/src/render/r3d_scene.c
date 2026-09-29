/* src/render/r3d_scene.c -- see r3d_scene.h. World frame: x right (lane
 * centres -240/0/+240, TR_PROJ_LANE_W), y up (ground 0), z depth (runner at
 * TR_PROJ_Z_RUNNER, spawn at TR_PROJ_Z_FAR). The world scrolls toward the
 * camera at the entities' own speed, so tiles, walls and entities agree. */
#include "r3d_scene.h"

#include "../game/state.h"
#include "../game/zone.h"
#include "../ipc/tr_memmap.h"
#include "meshes.h"
#include "proj.h"
#include "zones.h"

#include <string.h>

#define PI_F 3.14159265f
#define DEG  (PI_F / 180.0f)

/* Near ground is textured up to the first tile boundary at or past this
 * depth; from there on it is Gouraud mesh tiles, fogged per vertex. The
 * textured ground is fogged per sub-span through the same curve
 * (tr_r3d_fog_build), so the handover has no colour step at any fog depth. */
#define TEX_Z     2688.0f
#define FOG_START 600.0f
#define FOG_END   8800.0f
_Static_assert((int)FOG_START >= 65535 * (int)TR_CAM_Z_NEAR / (TR_FOG_LUT_N << TR_FOG_W_SHIFT),
	       "the ground fog table must reach FOG_START");

/* Horizon glow == fog colour, so the fogged far board, the band's ground
 * colour and the bottom of the sky gradient are one colour. */
#define GLOW_R 206
#define GLOW_G 112
#define GLOW_B 72
#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define ACCENT_IDX 15 /* the mesh palette's ACCENT (genmesh.py, genzone.py) */
/* Dusk sky, dithered (tr_bg_t): deep indigo -> magenta at SKY_MID_Q8 / 256 of
 * the way down -> the glow; stars fade in toward the top. */
#define SKY_TOP    RGB565(12, 8, 44)
#define SKY_MID    RGB565(104, 30, 100)
#define SKY_MID_Q8 150
/* The sun (far layer, rides with the camera in z) and its halo: HALO_K sun
 * radii, adding HALO_RGB at the centre. */
#define SUN_X    2600.0f
#define SUN_Y    2300.0f
#define SUN_DZ   30000.0f
#define SUN_R    2354.0f /* tr_mesh_sun's disc */
#define HALO_K   5.0f
#define HALO_RGB RGB565(255, 176, 96)

/* Walls past this depth use the low-poly LOD meshes (a few px tall there;
 * full detail would flood the horizon band's bin); entities too. */
#define LOD_Z 1800.0f
/* The small shoulder parts (tr_mesh_smd, tr_mesh_jumper) are drawn nearer
 * than this only: a few px tall past it, and ~20 tris each. */
#define SMALL_Z 1200.0f

#define RUNNER_Z ((float)TR_PROJ_Z_RUNNER)
/* P3c style knob (ponytail: tune on glass): squash-and-stretch per unit of
 * hip bob. */
#define SQUASH   0.012f
#define JUMP_H   170.0f

/* Crash (P6). The hit obstacle is drawn no nearer than CONTACT_DZ in front
 * of the runner: step.c judges the hit once the obstacle is already at the
 * runner line, where it would stand inside the runner ("ghost"); runner
 * front (TR_RUNNER_FRONT_Z, r3d_scene.h) + obstacle half depth (<= 24)
 * ~ 100 is touching.
 * Crash time runs SLOWMO_RATE x for the first SLOWMO_FRAMES frames (0.3 s
 * at 40 Hz). ponytail: calibration knobs, tune on glass. */
#define CONTACT_DZ    100.0f
#define SLOWMO_FRAMES 12u
#define SLOWMO_RATE   0.35f
#define KNOCK_BACK    35.0f  /* world units the runner is thrown back */
#define TUMBLE_RAD    1.25f  /* how far over backwards it tips */
#define FLASH_PX      16     /* red border thickness at the hit, px */
#define FLASH_FRAMES  10.0f  /* crash time the flash takes to fade */
#define FLASH_RGB     RGB565(230, 24, 16)
#define FLASH_WIRE    RGB565(170, 215, 255) /* a live wire: blue-white, not red */
#define JOLT_FRAMES   24.0f                 /* crash time a live wire shakes the runner */

/* World units the track scrolls at `tick` + phase/65536 ticks: an entity
 * covers (Z_FAR - Z_RUNNER) of depth in tr_runner_ground_y(H) model px, at
 * TR_SCROLL_PX model px per tick (step.c, proj.c). Exact floor of the real
 * product (phase 0 == the whole-tick value); two 64-bit divides a frame. */
static uint32_t scroll_units(uint32_t tick, uint16_t phase)
{
	uint64_t span = (uint64_t)(TR_PROJ_Z_FAR - TR_PROJ_Z_RUNNER) * TR_SCROLL_PX;
	uint64_t g    = (uint64_t)tr_runner_ground_y(TR_R3D_H);
	uint64_t all  = (uint64_t)tick * span;

	return (uint32_t)(all / g + ((all % g) * 65536u + (uint64_t)phase * span) / (g * 65536u));
}

uint32_t tr_scene_scroll(uint32_t tick, uint16_t phase)
{
	return scroll_units(tick, phase);
}

/* Sub-tick phase, Q0.16 -- only with TR_FLAG_PHASE: an old HE never wrote
 * those bytes (tr_mbox.h). */
static uint16_t phase_q16(const tr_frame_in_t *in)
{
	return (in->flags & TR_FLAG_PHASE) ? in->phase : 0u;
}

/* The same as a fraction of a tick. */
static float phase_f(const tr_frame_in_t *in)
{
	return (float)phase_q16(in) * (1.0f / 65536.0f);
}

/* Crash time in full-speed frames: slowed for the first SLOWMO_FRAMES. */
static float crash_time(const tr_frame_in_t *in)
{
	/* one crash tick a frame at any game pace (state.h tr_game_crash_frame) */
	float t = (float)in->crash_tick + (float)in->crash_frac * (1.0f / 65536.0f); /* + the 30 Hz fraction (P3d) */

	return t <= (float)SLOWMO_FRAMES ? t * SLOWMO_RATE : t - (float)SLOWMO_FRAMES * (1.0f - SLOWMO_RATE);
}

/* 40 Hz frames this frame stands for (tr_mbox.h hz; 0 from an old HE =
 * 40 Hz): every easing, blend, dip and particle step below is written per
 * 40 Hz frame and integrated over this many (P3d). */
static float frame_k(const tr_frame_in_t *in)
{
	return in->hz && in->hz < 40u ? 40.0f / (float)in->hz : 1.0f; /* > 40: never less than one */
}

/* Game steps a frame (tr_mbox.h pace_q8, set for the frame's own refresh);
 * 0 (an old HE, or a crash frame) = one 40 Hz frame's worth: 1.0 at 40 Hz. */
static float pace_f(const tr_frame_in_t *in)
{
	return in->pace_q8 ? (float)in->pace_q8 * (1.0f / 256.0f) : frame_k(in);
}

/* e^x for x <= 0 from IEEE basic ops (host == A32 bits): halved into
 * [-1/8, 0], a degree-6 Taylor polynomial (error < 3e-10 there), squared
 * back. Exponential easing at any frame time (keep^k = e^(k ln keep)). */
static float exp_neg(float x)
{
	int   n = 0;
	float r;

	while (x < -0.125f && n < 16) {
		x *= 0.5f, n++;
	}
	r = 1.0f + x * (1.0f + x * (0.5f + x * (1.0f / 6.0f + x * (1.0f / 24.0f + x * (1.0f / 120.0f + x * (1.0f / 720.0f))))));
	while (n-- > 0) {
		r *= r;
	}
	return r;
}

static float lane_x(uint8_t lane)
{
	return ((float)lane - (float)(TR_LANES - 1) * 0.5f) * (float)TR_PROJ_LANE_W;
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static float sinf_(float x)
{
	float s, c;

	tr_sincosf(x, &s, &c);
	return s;
}

static uint32_t xorshift(uint32_t *r)
{
	*r ^= *r << 13;
	*r ^= *r >> 17;
	*r ^= *r << 5;
	return *r;
}

/* Deterministic per-tile hash: which component stands on world tile `i`. */
static uint32_t hash(uint32_t i)
{
	i ^= i >> 16;
	i *= 0x7FEB352Du;
	i ^= i >> 15;
	i *= 0x846CA68Bu;
	return i ^ (i >> 16);
}

/* 8-bit channel lerp of an RGB565 colour toward `to` by a (0..256). */
static uint16_t mix565(uint16_t c, uint16_t to, uint32_t a)
{
	uint32_t r = ((c >> 11) * (256u - a) + (to >> 11) * a) >> 8;
	uint32_t g = (((c >> 5) & 63u) * (256u - a) + ((to >> 5) & 63u) * a) >> 8;
	uint32_t b = ((c & 31u) * (256u - a) + (to & 31u) * a) >> 8;

	return (uint16_t)(r << 11 | g << 5 | b);
}

/* The world's haze (maintainer on glass, three times: "start drawing
 * earlier"): the ground, walls and runner fog along the FOG_START..FOG_END
 * curve as ever up to view depth FOG_KNEE -- the whole textured near ground,
 * so its fog levels and the shoulder fade (fade_detail()) are unchanged --
 * and from there on only slowly, linearly to 1 at FOG_FAR: the board stays
 * readable out to GROUND_END, the skyline's foot, instead of melting into
 * the glow ~50 px under the horizon (FOG_END 8,800 alone), so the parts far
 * up the road stand ON the road. The ground stops at GROUND_END, ~72 %
 * fogged: the board meets the glow in a horizon line under the skyline.
 * ponytail: FOG_KNEE / FOG_FAR are the haze knobs, tune on glass. */
#define FOG_KNEE   3600.0f
#define FOG_FAR    72000.0f
#define GROUND_END 23800.0f /* world z: the skyline's foot (r3d_scene.h TR_SCENE_SKY_DZ ahead of the eye) */
_Static_assert((int)GROUND_END < TR_PROJ_Z_RUNNER - (int)TR_CAM_BACK + (int)TR_SCENE_SKY_DZ, "the board ends under the skyline");
/* The walls (shoulder parts, zone scenery) go on out there too, as
 * billboards past tr_scene_wall_bb_z (far_bb(), r3d_scene.h). */
float tr_scene_wall_bb_z = TR_WALL_BB_Z;

/* Lights: `l` for components (Lambert), `lg` for the board (straight up:
 * intensity 1 on the ground). Fog toward the horizon glow. */
static const tr_light_t light_l = {{0.46f, 0.78f, -0.42f}, 0.38f, {GLOW_R, GLOW_G, GLOW_B}, FOG_START, FOG_END, NULL,
				   FOG_KNEE, FOG_FAR};
/*
 * Entity fog (maintainer on glass: "the road obstacles start rendering when
 * they get close" / "You need to start earlier" / "start drawing earlier").
 * A part spawns at the far end of the visible road (state.h TR_SPAWN_Y, ~12 s
 * out) and grows and fades in over its first ENT_FADE_STEPS steps: from
 * nothing and the colour of the road under it (so it never pops, and rises
 * out of the road rather than out of a haze the road no longer has), to its
 * own colours under a light haze (ENT_HAZE_U: under 25 % of the glow),
 * rising with depth, so it reads as a distinct object from the far end in.
 *
 * Both in u space (fog amount = u (2 - u), tr_r3d_fog_amount's curve):
 * 1 - u = (1 - u_haze)(1 - u_fade), i.e. 1 - amount = (1 - haze)(1 - fade);
 * haze goes toward the glow, fade toward the road. One amount per entity,
 * from its world depth (camera independent).
 */
#define ENT_FADE_STEPS 5    /* 0.25 s at the 0.5x play pace (20 steps/s) */
#define ENT_HAZE_U     0.13f /* haze at spawn depth: 0.13 x 1.87 = 0.243 */

/* World depth an entity (or the gate) covers in one game step. */
static float dz_tick(void)
{
	return (float)((TR_PROJ_Z_FAR - TR_PROJ_Z_RUNNER) * TR_SCROLL_PX) / (float)tr_runner_ground_y(TR_R3D_H);
}
static const tr_light_t light_e = {{0.46f, 0.78f, -0.42f}, 0.38f, {GLOW_R, GLOW_G, GLOW_B}, 0.0f, 1.0f, NULL, 0.0f, 0.0f};

/*
 * Far LOD, size: past LOD_Z an entity is drawn larger about its base point,
 * by up to its kind's ENT_GROW (at spawn), linear in depth, so it is
 * recognisable (>= 16 px across, >= 6 px the other way) from 8 s out: the
 * obstacles ~2.3x there, the wires ~2x, the pickup -- a coin a third of a
 * lane wide -- ~4.7x (wider than its lane out there: its centre marks it). The growth comes in with the fade-in, from nothing at spawn.
 * Screen size k / z only ever grows as a part comes in (test_r3d_ent_lead.c:
 * a growth below ~9.8 keeps it so); nearer than LOD_Z nothing changes
 * (contact, collision and the crash are all near).
 */
#define ENT_GROW_OBST   2.1f
#define ENT_GROW_PICKUP 5.8f
#define ENT_GROW_WIRE   1.6f
#define PICKUP_BASE_Y   21.0f /* the coin's lowest underside (tr_mesh_via r 41 at y 72 - its 10 bob): it grows up off the road */

static float ent_grow(uint8_t kind)
{
	return kind == 2 ? ENT_GROW_PICKUP : kind == 3 ? ENT_GROW_WIRE : ENT_GROW_OBST;
}

/* How an entity is drawn this frame: its lit meshes' light (fog pinned to
 * one amount and colour), the two fog terms emissive parts combine with
 * their own haze factor and colours, and the far scale k about base. */
typedef struct {
	tr_light_t l;
	float      haze, fade; /* fog amounts, 0..1 */
	float      k, grown;   /* far scale; grown: 0 at spawn .. 1 once faded in */
	tr_v3_t    base;
	uint16_t   glow, road; /* what haze and fade go toward, RGB565 */
} ent_fog_t;

static float ent_u(float z, float *uh, float *uf)
{
	float zs = (float)tr_proj_depth_of_model_y(TR_SPAWN_Y);

	*uh = ENT_HAZE_U * clampf((z - FOG_START) / (zs - FOG_START), 0.0f, 1.0f);
	*uf = clampf((z - zs) / (ENT_FADE_STEPS * dz_tick()) + 1.0f, 0.0f, 1.0f);
	return 1.0f - (1.0f - *uh) * (1.0f - *uf);
}

float tr_scene_ent_fog(float z, float fk)
{
	float uh, uf;

	ent_u(z, &uh, &uf);
	return 1.0f - (1.0f - fk * uh * (2.0f - uh)) * (1.0f - uf * (2.0f - uf));
}

float tr_scene_ent_scale(float z, uint8_t kind)
{
	float zs = (float)tr_proj_depth_of_model_y(TR_SPAWN_Y), uh, uf;

	ent_u(z, &uh, &uf);
	return (1.0f - uf) * (1.0f + ent_grow(kind) * clampf((z - LOD_Z) / (zs - LOD_Z), 0.0f, 1.0f));
}

/* ent_fog_t for an entity of `kind` at (x, 0, z), camera depth vz, from le
 * (the zone's entity light, zone_ctx(): its glow and tinted palette) and
 * road, the board's colour under it: the light's fog ramp is stretched over
 * ENT_FOG_SPAN so every vertex of the part (a few hundred units deep at
 * most) gets the same amount, toward the one colour haze-then-fade leaves:
 * lerp(lerp(c, glow, haze), road, fade) == lerp(c, fog, amount). */
#define ENT_FOG_SPAN 1.0e6f
static ent_fog_t ent_fog(const tr_light_t *le, float x, float z, float vz, uint8_t kind, uint16_t road)
{
	ent_fog_t f = {*le, 0.0f, 0.0f, tr_scene_ent_scale(z, kind), 0.0f, {x, kind == 2 ? PICKUP_BASE_Y : 0.0f, z},
		       RGB565(le->fog[0], le->fog[1], le->fog[2]), road};
	float     uh, uf, u = ent_u(z, &uh, &uf);

	f.grown       = 1.0f - uf;
	f.l.fog_start = vz - u * ENT_FOG_SPAN;
	f.l.fog_end   = f.l.fog_start + ENT_FOG_SPAN;
	f.haze        = uh * (2.0f - uh);
	f.fade        = uf * (2.0f - uf);
	if (f.fade > 0.0f) {
		float    a = 1.0f - (1.0f - f.haze) * (1.0f - f.fade), wg = f.haze * (1.0f - f.fade) / a, wr = f.fade / a;
		uint32_t rd[3] = {(uint32_t)(road >> 11) << 3, (uint32_t)((road >> 5) & 63u) << 2, (uint32_t)(road & 31u) << 3};

		for (int c = 0; c < 3; c++) {
			f.l.fog[c] = (uint8_t)((float)le->fog[c] * wg + (float)rd[c] * wr + 0.5f);
		}
	}
	return f;
}
static const tr_light_t light_lg = {{0.0f, 1.0f, 0.0f}, 0.0f, {GLOW_R, GLOW_G, GLOW_B}, FOG_START, FOG_END, NULL,
				    FOG_KNEE, FOG_FAR};
/* The zone scenery's emissive parts (neon strips, antenna rings: near only)
 * fog on the plain curve out to here (unlit(): not the world's far knee). */
#define FAR_FOG_END 12800.0f

/* The fogged palette rows of the four ground slots (TR_SCENE_TEX_*: the
 * zone before the gate, the zone beyond it), bound by bind_ground(). */
static uint16_t fog_pal[8][TR_FOG_LEVELS * TR_FOG_PAL_MAX];
static uint16_t avg_pal[4][TR_FOG_PAL_MAX]; /* the far road slots' colours (avg_tex()), [half * 2 + lane/board] */
static uint32_t avg_n[4];
/* The camera-side zone's two textures expanded to RGB565: the near ground --
 * fog level 0, ~60 % of the ground's pixels -- keeps the plain texel path
 * (one load a pixel, not the palette's two: r3d_raster.c tex_run_noz()). The
 * zone beyond the gate draws level 0 through its palette: nearby only for
 * the ~1 s the gate passes. 64 KiB: in the renderer image a fixed SRAM0
 * region (tr_memmap.h, WB in its table), outside its 512 KiB budget. */
_Static_assert(2u * TR_TEX_DIM * TR_TEX_DIM * 2u == TR_MEM_A32_ZTEX_SIZE, "tr_memmap.h TR_MEM_A32_ZTEX_SIZE");
/* The four slots' textures unpacked from zones.h's 4 bpp to r3d.h's index
 * bytes (entry * 2), and the far road's two (avg_tex()): 6 x 16 KiB, a
 * second fixed SRAM0 region (tr_memmap.h
 * TR_MEM_A32_ZIDX) -- the packed art is what fits the image budget. */
_Static_assert(6u * TR_TEX_DIM * TR_TEX_DIM == TR_MEM_A32_ZIDX_SIZE, "tr_memmap.h TR_MEM_A32_ZIDX_SIZE");
#if RENDER_A32
#define near_tex ((uint16_t(*)[TR_TEX_DIM * TR_TEX_DIM])TR_MEM_A32_ZTEX)
#define zidx     ((uint8_t(*)[TR_TEX_DIM * TR_TEX_DIM])TR_MEM_A32_ZIDX)
#else
static uint16_t near_tex[2][TR_TEX_DIM * TR_TEX_DIM];
static uint8_t  zidx[6][TR_TEX_DIM * TR_TEX_DIM];
#endif

/*
 * Shoulder shimmer: the board art (1-3 texel traces, pads, silkscreen) is
 * sampled nearest, and past ~700 deep a shoulder pixel spans 1.3 x 3 texels
 * (u x v at 1500) -- which texel it lands on changes every frame, so the
 * detail flickers in stripes along the shoulders, the screen's sides
 * (measured, SSAA-referenced: 8-16 luminance / frame, 2-3x the lanes, 6x the
 * parts). A mip level would cost a texture; the fog palette rows already
 * stand for a depth each, so each row also fades the art toward its
 * texel-weighted mean colour -- what a box filter leaves of detail finer
 * than the pixel -- keeping (SOFT_Z / z)^2 of the contrast (z: the row's
 * nearest depth, from the fog LUT; squared like the texels a pixel row
 * spans). Level 0 (nearer than ~670, magnified) is the raw texture, and
 * level 1 starts at full contrast, so the fade has no step. ponytail: a
 * calibration knob; a real mip level if the fade reads as blur on glass.
 */
#define SOFT_Z 700.0f
/* Fades the fogged palette rows of zone texture (zone, j) toward what its
 * pixels average to at each row's depth: colour k toward its neighbourhood's
 * mix, keeping (SOFT_Z / z)^2 of the contrast. A thin line or a
 * dotted row -- the canyon's bus lines and dot rows, the die's cell rows --
 * melts into what surrounds it; a wide stripe stays itself. */
static void fade_detail(uint16_t *pal, uint32_t n, uint32_t zone, uint32_t j)
{
	const tr_ztex_t *t              = &tr_ztex[zone][j];
	float            zl[TR_FOG_LEVELS] = {0}, w[16][16];

	/* w[k][c]: the share of colour c round a texel of colour k -- the 5 x 5
	 * box on the lanes (zones.h nb, counted by tools/genzone.py), the whole
	 * texture on the shoulders (busy art edge to edge: cnt, its mean) */
	for (uint32_t k = 0; k < n; k++) {
		float sum = 0.0f;

		for (uint32_t c = 0; c < n; c++) {
			w[k][c] = (float)(j ? t->cnt[c] : t->nb[k * 16u + c]);
			sum += w[k][c];
		}
		for (uint32_t c = 0; c < n; c++) {
			w[k][c] /= sum > 0.0f ? sum : 1.0f;
		}
	}
	for (uint32_t i = TR_FOG_LUT_N; i-- > 0;) { /* near to far: a level's first bin is its nearest */
		uint8_t lv = tr_r3d_fog_lut[i];

		if (zl[lv] == 0.0f) {
			zl[lv] = 65535.0f * TR_CAM_Z_NEAR / (((float)i + 0.5f) * (float)(1u << TR_FOG_W_SHIFT));
		}
	}
	for (uint32_t lv = 1; lv < TR_FOG_LEVELS; lv++) {
		uint16_t *row = &pal[lv * n], in[TR_FOG_PAL_MAX];
		uint32_t  a;

		if (zl[lv] <= SOFT_Z) {
			continue; /* unused level (0), or still sharp */
		}
		a = (uint32_t)(256.0f * (SOFT_Z / zl[lv]) * (SOFT_Z / zl[lv])); /* contrast kept, Q8 */
		memcpy(in, row, n * sizeof(in[0]));
		for (uint32_t k = 0; k < n; k++) {
			float mix[3] = {0.0f, 0.0f, 0.0f};

			for (uint32_t c = 0; c < n; c++) {
				mix[0] += w[k][c] * (float)(in[c] >> 11);
				mix[1] += w[k][c] * (float)((in[c] >> 5) & 63u);
				mix[2] += w[k][c] * (float)(in[c] & 31u);
			}
			uint32_t ch[3] = {in[k] >> 11, (in[k] >> 5) & 63u, in[k] & 31u};

			for (int c = 0; c < 3; c++) {
				ch[c] = (ch[c] * 256u * a + (uint32_t)(mix[c] * 256.0f + 0.5f) * (256u - a) + 32768u) >> 16;
			}
			row[k] = (uint16_t)(ch[0] << 11 | ch[1] << 5 | ch[2]);
		}
	}
}

/* P12 time: tq, the render time in 1/16 ticks (tick + sub-tick phase) --
 * integer, so a pattern never drifts with a large tick. */
static uint32_t time_q4(const tr_frame_in_t *in)
{
	return in->tick * 16u + (uint32_t)(phase_q16(in) >> 12);
}

/* ------------------------------------------------------ world zones (P15)
 * The look of each zone (src/game/zone.h order): sky gradient stops, the
 * horizon glow (= fog colour), the sun or moon and its halo, stars, the far
 * silhouette, and the tint its obstacles take on. Everything a zone draws
 * is a pure function of the packet's zone + gate_y (tr_scene_zone()). */
typedef struct {
	uint16_t         top, mid;
	uint8_t          mid_q8, stars;
	uint8_t          glow[3];
	uint16_t         halo;    /* RGB565 added at the halo's centre */
	float            halo_r;  /* halo radius, world units at SUN_DZ (HALO_K sun radii on the board) */
	const tr_mesh_t *sun, *sky;
	uint16_t         tint;    /* obstacles lerp toward this ... */
	uint8_t          tint_a;  /* ... by tint_a / 256 (0: the board's own colours) */
} zlook_t;

static const zlook_t zlook[TR_ZONES] = {
	/* the board at dusk: indigo -> magenta -> the orange glow, stars, the sun */
	{SKY_TOP, SKY_MID, SKY_MID_Q8, 1, {GLOW_R, GLOW_G, GLOW_B}, HALO_RGB, HALO_K * SUN_R, &tr_mesh_sun,
	 &tr_mesh_skyline, 0, 0},
	/* CPU die city: under a heat spreader -- dark nickel overhead, brushed
	 * silver, a warm white glow; no stars, the die light */
	{RGB565(46, 50, 62), RGB565(150, 156, 170), 120, 0, {206, 192, 172}, RGB565(255, 244, 216), 3.2f * SUN_R,
	 &tr_mesh_sun, &tr_zmesh_sky_die, RGB565(140, 110, 220), 70},
	/* memory canyon: deep blue to a teal glow, a cold light */
	{RGB565(4, 10, 28), RGB565(16, 62, 96), 150, 1, {68, 150, 168}, RGB565(110, 230, 255), 3.0f * SUN_R,
	 &tr_mesh_sun, &tr_zmesh_sky_mem, RGB565(60, 200, 230), 80},
	/* antenna field: green-teal dusk to a golden glow */
	{RGB565(10, 30, 36), RGB565(66, 104, 58), 150, 1, {214, 188, 100}, RGB565(255, 220, 110), 4.5f * SUN_R,
	 &tr_mesh_sun, &tr_zmesh_sky_rf, RGB565(170, 230, 90), 70},
	/* neon city: night, a purple haze, the moon */
	{RGB565(2, 2, 12), RGB565(28, 10, 50), 170, 1, {80, 30, 108}, RGB565(120, 140, 255), 3.0f * SUN_R,
	 &tr_zmesh_moon, &tr_zmesh_sky_neon, RGB565(255, 60, 220), 90},
};

/* The look blends over the gate's pass: from ZONE_BLEND_DZ before the
 * runner reaches it to ZONE_BLEND_DZ after -- ~1 s at the play pace (20
 * steps/s x ~89 units a step). zone.h's TR_ZONE_GATE_TAIL keeps the gate
 * reported until the blend is done (test_r3d_zones checks). */
#define ZONE_BLEND_DZ 900.0f

static float smooth01(float x);

float tr_scene_zone(const tr_frame_in_t *in, uint8_t *near, uint8_t *far, float *gate_z)
{
	uint8_t z = (in->flags & TR_FLAG_ZONE) ? (uint8_t)(in->zone % TR_ZONES) : 0u;

	*near = *far = z;
	*gate_z = -1e30f;
	if (!(in->flags & TR_FLAG_ZONE) || in->gate_y == TR_ZONE_NO_GATE) {
		return 0.0f;
	}
	/* zone.h: the runner enters the next zone on the step the gate reaches
	 * its line; the packet's y (not the phase-interpolated depth) says
	 * which side of it we are. */
	if (in->gate_y < tr_runner_ground_y(TR_R3D_H)) {
		*far = (uint8_t)((z + 1u) % TR_ZONES);
	} else {
		*near = (uint8_t)((z + TR_ZONES - 1u) % TR_ZONES);
	}
	*gate_z = (float)tr_proj_depth_of_model_y(in->gate_y) - phase_f(in) * dz_tick();
	return smooth01((RUNNER_Z + ZONE_BLEND_DZ - *gate_z) * (0.5f / ZONE_BLEND_DZ));
}

/* The frame's zone context: the view, the blended look's glow, and every
 * light the build shades with -- per zone for the scenery (its palette),
 * the ground, the far layer and the obstacles (tinted). */
typedef struct {
	uint8_t    near, far;
	float      t, gate_z;
	uint8_t    glow[3];
	tr_light_t l[2], lg[2]; /* scenery / ground, [0] near the gate's camera side, [1] beyond */
	tr_light_t le;          /* entities (ent_fog() pins its fog per part): the tinted obstacle palette */
	tr_light_t lr;          /* the runner: its own colours */
	uint16_t   obst[16];    /* le.pal's storage */
	uint16_t   pal[2][16];  /* the two zones' palettes, animated (zone_lights()) */
} zctx_t;

static uint32_t q8(float t)
{
	return (uint32_t)(t * 256.0f + 0.5f);
}

/* The living lights of a zone palette p (zone zn) at render time tq (1/16
 * ticks): the die towers' aviation lights and the antenna beacons blink,
 * the DIMMs' activity LEDs flicker, the neon hums (each colour on its own
 * slow swell) and the magenta tube stutters now and then. A pure function
 * of tq: a frame rebuilt is the same frame. ponytail: rates are tune-on-
 * glass knobs. */
static void zone_lights(uint16_t *p, uint8_t zn, uint32_t tq)
{
	const uint16_t off = RGB565(20, 16, 20);

	switch (zn) {
	case TR_ZONE_DIE: /* genzone.py DIE LIGHT: 5 of every 32 ticks */
		p[14] = ((tq >> 4) & 31u) < 5u ? p[14] : mix565(p[14], off, 210);
		break;
	case TR_ZONE_MEM: /* MEM LED: random activity, a new state every half tick */
		p[13] = (hash(tq >> 3) & 3u) != 0u ? p[13] : mix565(p[13], off, 200);
		break;
	case TR_ZONE_RF: /* RF BEACON: 1 s on, 1 s off at the play pace */
		p[10] = ((tq >> 4) % 40u) < 20u ? p[10] : mix565(p[10], off, 190);
		break;
	case TR_ZONE_NEON: /* NEON_C / _M / _V swell 75 .. 100 %, M stutters */
		for (int k = 0; k < 3; k++) {
			/* ~0.021 / 0.028 / 0.035 rad per tq as Q16 turns: no jump when
			 * a float tq would lose precision or a mask would wrap */
			uint32_t turn = (tq * (219u + 73u * (uint32_t)k)) & 0xFFFFu;
			float    a    = 0.5f + 0.5f * sinf_((float)turn * (2.0f * PI_F / 65536.0f) + 2.1f * (float)k);

			p[9 + k] = mix565(p[9 + k], RGB565(0, 0, 0), (uint32_t)(64.0f * a));
		}
		if ((hash(tq >> 5) & 31u) == 0u) {
			p[10] = mix565(p[10], off, 180);
		}
		break;
	default:
		break;
	}
}

static void zone_ctx(const tr_frame_in_t *in, zctx_t *z)
{
	z->t = tr_scene_zone(in, &z->near, &z->far, &z->gate_z);
	const zlook_t *a = &zlook[z->near], *b = &zlook[z->far];

	for (int c = 0; c < 3; c++) {
		z->glow[c] = (uint8_t)((float)a->glow[c] + ((float)b->glow[c] - (float)a->glow[c]) * z->t + 0.5f);
	}
	for (int k = 0; k < 2; k++) {
		uint8_t zn = k ? z->far : z->near;

		z->l[k]  = light_l;
		z->lg[k] = light_lg;
		memcpy(z->l[k].fog, z->glow, 3);
		memcpy(z->lg[k].fog, z->glow, 3);
		z->l[k].pal = z->lg[k].pal = NULL;
		if (tr_zpal[zn] != NULL) {
			memcpy(z->pal[k], tr_zpal[zn], sizeof(z->pal[k]));
			zone_lights(z->pal[k], zn, time_q4(in));
			z->l[k].pal = z->lg[k].pal = z->pal[k];
		}
	}
	z->le = light_e; /* ent_fog() sets its fog amount per part */
	memcpy(z->le.fog, z->glow, 3);
	z->lr = light_l; /* the runner: light_l's curve, its own / the board palette */
	memcpy(z->lr.fog, z->glow, 3);
	/* Obstacles keep the board's parts, tinted toward the zone they are in
	 * (the look's blend). */
	uint32_t ta = (uint32_t)((float)a->tint_a + ((float)b->tint_a - (float)a->tint_a) * z->t + 0.5f);

	if (ta != 0u) {
		uint16_t tint = a->tint_a == 0 ? b->tint : b->tint_a == 0 ? a->tint : mix565(a->tint, b->tint, q8(z->t));

		for (int k = 0; k < 16; k++) {
			z->obst[k] = mix565(tr_r3d_palette[k], tint, ta);
		}
		z->le.pal = z->obst;
	}
}

/* 1: a ground or scenery piece at world depth zw is beyond the gate (the
 * incoming zone, index 1 of zctx_t's lights). */
static int beyond(const zctx_t *z, float zw)
{
	return z->near != z->far && zw >= z->gate_z;
}

/*
 * The far road's texture (slot TR_SCENE_TEX_*_AVG + j, or _AVG_FAR + j: the
 * zone past a gate, half 1 of the same rows): zone texture t as the far road
 * shows it, three levels in three bands of a half's 64 rows (a far road run
 * reads one row, avg_v()). Past the near ground a pixel row spans dozens of
 * texel rows, so nearest sampling picks a different one every frame --
 * dashes and pads would sparkle. Averaged down each column (and over three
 * columns), every row of a level is the same: what a box filter leaves of
 * the texture -- the lane lines, rails and traces along the road, the dashes
 * as a paler line -- and it does not move as the board scrolls. But a lane
 * is 50 px wide there, then 20, then 7, and a 2-texel lane line a fraction of
 * a pixel: each level widens every bright line to ~1 px at its far end (each
 * column takes the brightest averaged colour within +-AVG_HW[k] columns), so
 * the lanes stay marked out to the skyline. Its colours, merged to <=
 * TR_FOG_PAL_MAX, go through the fog palette like the near ground's.
 * tools/genzone.py builds the rows (zones.h avg, avg_pal: +-1, 3, 9 columns
 * widened); a bind copies them.
 */
#define AVG_LEVELS 3
#define AVG_HALF   (TR_TEX_DIM / 2u)
static const float    avg_z[AVG_LEVELS - 1] = {4000.0f, 8000.0f}; /* world z the next level takes over */

/* A far road run's v for level k of half h: the middle of its band of rows
 * (UVX8: 0x1000 a texture height, 32 a row). */
static uint16_t avg_v(uint32_t k, uint32_t h)
{
	return (uint16_t)((h * AVG_HALF + (k * AVG_HALF + AVG_HALF / 2u) / AVG_LEVELS) * 32u + 16u);
}

static void avg_tex(const tr_ztex_t *t, uint32_t j, uint32_t h)
{
	for (uint32_t v = 0; v < AVG_HALF; v++) {
		memcpy(&zidx[TR_SCENE_TEX_LANE_AVG + j][(h * AVG_HALF + v) * TR_TEX_DIM], &t->avg[(v * AVG_LEVELS / AVG_HALF) * TR_TEX_DIM],
		       TR_TEX_DIM);
	}
	memcpy(avg_pal[h * 2u + j], t->avg_pal, t->avg_n * sizeof(t->avg_pal[0]));
	avg_n[h * 2u + j] = t->avg_n;
}

/* The far road's half h now shows zone zn (held[h]): the camera-side half
 * takes over the far half's rows when the runner crosses into its zone (a
 * copy, no recompute, no step on screen); anything else is averaged anew. */
static void avg_bind(uint32_t h, uint32_t zn, uint32_t *held)
{
	if (held[h] == zn) {
		return;
	}
	for (uint32_t j = 0; j < 2u; j++) {
		if (h == 0u && held[1] == zn) {
			memcpy(&zidx[TR_SCENE_TEX_LANE_AVG + j][0], &zidx[TR_SCENE_TEX_LANE_AVG + j][AVG_HALF * TR_TEX_DIM],
			       AVG_HALF * TR_TEX_DIM);
			memcpy(avg_pal[j], avg_pal[2u + j], sizeof(avg_pal[0]));
			avg_n[j] = avg_n[2u + j];
		} else {
			avg_tex(&tr_ztex[zn][j], j, h);
		}
	}
	held[h] = zn;
}

/*
 * Points the four ground slots at the textures of the zone before the gate
 * (TR_SCENE_TEX_LANE/BOARD) and beyond it (_FAR), fogged toward the frame's
 * glow, the shoulders' detail faded (fade_detail()). Rebuilt only when the
 * zones or the glow change (a transition, ~1 s). Build parts 0 and 1 only:
 * part 2 runs concurrently on the other core and never reads the slots; the
 * raster reads them after both. ponytail: one set of slots, so the last
 * scene built owns them.
 */
static void bind_ground(const zctx_t *z)
{
	static uint32_t key = 0xFFFFFFFFu, expanded = 0xFFu, held[4] = {0xFFu, 0xFFu, 0xFFu, 0xFFu}, held_avg[2] = {0xFFu, 0xFFu};
	uint32_t        k   = (uint32_t)z->near | (uint32_t)z->far << 4 | (uint32_t)z->glow[0] << 8 |
			 (uint32_t)z->glow[1] << 16 | (uint32_t)z->glow[2] << 24;
	int             ok  = k == key;

	for (int slot = 0; slot < 8; slot++) { /* and nothing re-pointed a slot since (a test, the golden DL) */
		ok = ok && tr_r3d_tex[slot] == (slot < 2 ? near_tex[slot] : NULL) && tr_r3d_tex_fog[slot].pal == fog_pal[slot] &&
		     tr_r3d_tex_fog[slot].idx == zidx[slot < 6 ? slot : slot - 2];
	}
	if (ok) {
		return;
	}
	key = k;
	for (uint32_t slot = 0; slot < 4u; slot++) { /* a slot's zone changed: unpack its 16 K texels */
		uint32_t zn = slot < 2u ? z->near : z->far;

		if (held[slot] != zn) {
			const tr_ztex_t *t = &tr_ztex[zn][slot & 1u];

			for (uint32_t i = 0; i < TR_TEX_DIM * TR_TEX_DIM; i++) {
				zidx[slot][i] = (uint8_t)(2u * tr_ztex_at(t, i));
			}
			held[slot] = zn;
		}
	}
	avg_bind(0u, z->near, held_avg); /* the far road's: camera side ... */
	if (z->far != z->near) {
		avg_bind(1u, z->far, held_avg); /* ... and past the gate */
	}
	if (expanded != z->near) { /* once a zone: 2 x 16 K texels */
		for (int j = 0; j < 2; j++) {
			const tr_ztex_t *t = &tr_ztex[z->near][j];

			for (uint32_t i = 0; i < TR_TEX_DIM * TR_TEX_DIM; i++) {
				near_tex[j][i] = t->pal[zidx[j][i] >> 1];
			}
		}
		expanded = z->near;
	}
	for (int slot = 0; slot < 4; slot++) {
		const tr_ztex_t *t = &tr_ztex[slot < 2 ? z->near : z->far][slot & 1];

		/* the far pair index-only: every level through the palette (r3d.h) */
		tr_r3d_tex[slot] = slot < 2 ? near_tex[slot] : NULL;
		tr_r3d_fog_pal(&z->lg[0], t->pal, t->n, fog_pal[slot]);
		fade_detail(fog_pal[slot], t->n, slot < 2 ? z->near : z->far, (uint32_t)slot & 1u);
		tr_r3d_tex_fog[slot] = (tr_tex_fog_t){zidx[slot], fog_pal[slot], t->n};
	}
	for (int a = 0; a < 4; a++) { /* the far road: index-only, no fade_detail (the averaging is its filter) */
		int slot = TR_SCENE_TEX_LANE_AVG + a;

		tr_r3d_tex[slot] = NULL;
		tr_r3d_fog_pal(&z->lg[0], avg_pal[a], avg_n[a], fog_pal[slot]);
		tr_r3d_tex_fog[slot] = (tr_tex_fog_t){zidx[TR_SCENE_TEX_LANE_AVG + (a & 1)], fog_pal[slot], avg_n[a]};
	}
}

void tr_scene_init(tr_scene_t *s)
{
	zctx_t z;

	*s                     = (tr_scene_t){0};
	s->rng                 = 0x2545F491u;
	s->cam_x = s->runner_x = lane_x(1);
	s->prev_lane           = 1;
	s->squash              = 1.0f;
	s->face.open           = 1.0f;
	/* One fog curve for every zone (only the colour changes). */
	tr_r3d_fog_lut_build(&light_lg);
	zone_ctx(&(tr_frame_in_t){0}, &z);
	bind_ground(&z);
}

/* Eases *w toward `to` by `rate` a frame. */
static void ease(float *w, float to, float rate)
{
	*w = *w < to ? (*w + rate > to ? to : *w + rate) : (*w - rate < to ? to : *w - rate);
}

/*
 * Frame-rate independence (P3d). A frame stands for k 40 Hz frames of real
 * time (frame_k). Whatever the packet changed (a lane, a flag) happened
 * somewhere in that time; at every rate it is taken to have happened ONE
 * 40 Hz frame before this one -- exactly what the 40 Hz build always did --
 * so the k - 1 frames before it still ease toward the previous frame's
 * target. Then a 30 Hz and a 40 Hz stream agree at equal real times, and
 * 40 Hz (k = 1) is bit for bit the old per-frame easing.
 */
static void ease_k(float *w, float was, float to, float rate, float k)
{
	ease(w, was, rate * (k - 1.0f));
	ease(w, to, rate);
}

/* The same for an exponential slide of `rate` (0.35: 35 % of the way) a
 * 40 Hz frame, ln_keep = ln(1 - rate): 1 - (1 - rate)^(k - 1) toward `was`. */
static void slide_k(float *x, float was, float to, float rate, float ln_keep, float k)
{
	*x += (was - *x) * (1.0f - exp_neg((k - 1.0f) * ln_keep));
	*x += (to - *x) * rate;
}

static void mix(float *ch, const float *to, float w)
{
	for (int c = 0; c < TR_ANIM_CH; c++) {
		ch[c] += (to[c] - ch[c]) * w;
	}
}

static float smooth01(float x)
{
	x = clampf(x, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

/* ------------------------------------------------------------ character (P16)
 * The character drawn: the packet's with TR_FLAG_CHAR (an old HE: Probe). */
_Static_assert(TR_CHARS == TR_CHAR_N, "meshes.h characters != tr_mbox.h TR_CHAR_N");
_Static_assert(TR_SCARF_PTS >= TR_SCARF_MAX + 1, "r3d_scene.h TR_SCARF_PTS < meshes.h TR_SCARF_MAX + 1");

/* The runner's level of detail: the high one, the P16 set under the bench's
 * TR_LOD_NEAR bit (the runner is always at the same depth: no distance LOD). */
static int runner_lod(const tr_scene_t *s)
{
	return (s->quality & TR_LOD_NEAR) ? 1 : 0;
}

static int char_of(const tr_frame_in_t *in)
{
	return (in->flags & TR_FLAG_CHAR) ? tr_rig_char_id(in->character) : 0;
}

/* Channel masks the layered poses blend through (bit c: channel c). A
 * reaction over the run moves the upper body only: the legs keep the run
 * cycle and the foot lock re-plants them under whatever the hips do. */
#define CH_BIT(c)   (1u << (c))
#define ARM_BITS(s) (0xFu << (TR_ANIM_SIDE0 + TR_ANIM_SIDE_N * (s) + 4))
#define MASK_UPPER  (CH_BIT(1) | CH_BIT(3) | CH_BIT(4) | CH_BIT(5) | CH_BIT(6) | CH_BIT(7) | CH_BIT(8) | ARM_BITS(0) | ARM_BITS(1))
#define MASK_LOOK   (CH_BIT(5) | CH_BIT(6) | CH_BIT(7))
_Static_assert(TR_ANIM_SIDE0 + 2 * TR_ANIM_SIDE_N <= 32, "channel masks are 32 bits");

static void mix_mask(float *ch, const float *to, float w, uint32_t mask)
{
	for (int c = 0; c < TR_ANIM_CH; c++) {
		if (mask & CH_BIT(c)) {
			ch[c] += (to[c] - ch[c]) * w;
		}
	}
}

/* 1 inside [0, d] with smooth edges `in` and `out` long (seconds). */
static float env(float a, float in, float d, float out)
{
	return smooth01(a / in) * smooth01((d - a) / out);
}

/* x mod m for x >= 0 (no libm). */
static float fmodp(float x, float m)
{
	return x - m * (float)(int32_t)(x / m);
}

/* The reaction the frame shows (TR_REACT_* after the attract flourish
 * swap): in attract, one pass in five becomes a spin and one a fist pump
 * -- the demo shows off its moves now and then. */
static uint8_t react_kind(const tr_frame_in_t *in)
{
	uint8_t k = (in->flags & TR_FLAG_CHAR) ? in->react : TR_REACT_NONE;

	if (k == TR_REACT_PASS && (in->flags & TR_FLAG_ATTRACT_ACTIVE)) {
		k = in->react_seq % 5u == 2u ? TR_REACT_COMBO : in->react_seq % 5u == 4u ? TR_REACT_PICKUP : k;
	}
	return k;
}

/* Reaction lengths, seconds: glance, pump, stumble, spin. */
#define REACT_GLANCE_S 0.9f
#define REACT_PUMP_S   0.75f
#define REACT_NEAR_S   0.7f
#define REACT_SPIN_S   0.65f

/*
 * The reaction layer over the run (P16), a pure function of the packet:
 * the latest event's pose (glance back toward the side it went by, fist
 * pump, stumble, arms-out spin) blended over `ch` through its channel mask
 * by an envelope of its age react_ms, plus the eyes it makes and the spin
 * it turns. Returns the layer's weight (0: nothing showing).
 */
static float react_layer(const tr_frame_in_t *in, float *ch, tr_face_t *f, float *spin)
{
	uint8_t k    = react_kind(in);
	float   a    = (float)in->react_ms * 0.001f, w = 0.0f, pose[TR_ANIM_CH];
	float   side = (int8_t)in->react_side < 0 ? -1.0f : 1.0f;

	*spin = 0.0f;
	if (k == TR_REACT_PASS) {
		w = env(a, 0.15f, REACT_GLANCE_S, 0.3f);
		memcpy(pose, tr_rig_pose(TR_ANIM_POSE_GLANCE), sizeof(pose));
		pose[5] *= side, pose[7] *= side; /* the posed glance is over the right shoulder */
		mix_mask(ch, pose, w, MASK_LOOK);
		f->look_x = 2.5f * side * w;
	} else if (k == TR_REACT_PICKUP) {
		w = env(a, 0.08f, REACT_PUMP_S, 0.25f);
		memcpy(pose, tr_rig_pose(TR_ANIM_POSE_PUMP), sizeof(pose));
		pose[TR_ANIM_SIDE0 + TR_ANIM_SIDE_N + 5] -= 34.0f * (1.0f - sinf_(2.0f * PI_F * a / 0.32f)); /* pumping */
		mix_mask(ch, pose, w, MASK_UPPER);
		f->happy = env(a, 0.08f, 0.8f, 0.2f);
	} else if (k == TR_REACT_NEAR) {
		w = env(a, 0.06f, REACT_NEAR_S, 0.35f);
		mix_mask(ch, tr_rig_pose(TR_ANIM_POSE_STUMBLE), w, MASK_UPPER);
		f->squint = env(a, 0.04f, 0.6f, 0.2f);
		f->wide   = env(a - 0.45f, 0.05f, 0.35f, 0.15f);
	} else if (k == TR_REACT_COMBO) {
		w = env(a, 0.08f, REACT_SPIN_S, 0.2f);
		mix_mask(ch, tr_rig_pose(TR_ANIM_POSE_SPIN), w, MASK_UPPER);
		*spin    = a < REACT_SPIN_S ? 2.0f * PI_F * smooth01((a - 0.04f) / (REACT_SPIN_S - 0.12f)) : 0.0f;
		f->happy = env(a, 0.08f, 1.3f, 0.3f);
	}
	return w;
}

/*
 * The idle set (P16, TR_FLAG_IDLE: the attract lobby, no run) at idle time
 * t seconds, a full-body pose over the standing one: turned to face the
 * camera, then on a 9.6 s loop -- a big overhead wave at the passer-by
 * (happy eyes), a look around, a long stretch (eyes shut), a bored slouch
 * tapping one foot (half-lidded, eyes wandering), a look around again.
 * Writes the pose and the eyes; returns the yaw it stands at.
 */
#define IDLE_LOOP0 1.4f
#define IDLE_LOOP  9.6f
static float idle_set(float t, float *ch, tr_face_t *f)
{
	float u = t < IDLE_LOOP0 ? t : IDLE_LOOP0 + fmodp(t - IDLE_LOOP0, IDLE_LOOP);
	float ww = env(u - 1.4f, 0.35f, 2.4f, 0.35f), ws = env(u - 4.6f, 0.4f, 2.0f, 0.4f), wl = env(u - 7.2f, 0.4f, 3.2f, 0.4f);
	float wave[TR_ANIM_CH], tap = sinf_(2.0f * PI_F * 2.5f * t);
	int   r = TR_ANIM_SIDE0 + TR_ANIM_SIDE_N; /* the right side's channels */

	memcpy(ch, tr_rig_pose(TR_ANIM_POSE_STAND), sizeof(float) * TR_ANIM_CH);
	memcpy(wave, tr_rig_pose(TR_ANIM_POSE_WAVE), sizeof(wave));
	wave[r + 6] += 20.0f * sinf_(2.0f * PI_F * 1.8f * t);        /* abduction: side to side */
	wave[r + 5] += 12.0f * sinf_(2.0f * PI_F * 1.8f * t + 1.1f); /* the forearm follows */
	mix(ch, wave, ww);
	mix(ch, tr_rig_pose(TR_ANIM_POSE_STRETCH), ws);
	mix(ch, tr_rig_pose(TR_ANIM_POSE_SLOUCH), wl);
	tap = tap > 0.0f ? tap * wl : 0.0f; /* the right foot lifts and drops, 2.5 taps a second */
	ch[r + 0] -= 12.0f * tap, ch[r + 1] += 20.0f * tap, ch[r + 2] -= 8.0f * tap;
	ch[7] += 24.0f * sinf_(2.0f * PI_F * 0.23f * t) * (1.0f - ww - ws - wl > 0.0f ? 1.0f - ww - ws - wl : 0.0f);
	f->happy  = 0.9f * ww + 0.3f * ws;
	f->open  *= (1.0f - 0.9f * ws) * (1.0f - 0.45f * wl);
	f->look_x = 3.0f * sinf_(0.9f * t) * wl;
	return PI_F * smooth01((t - 0.3f) / 0.8f);
}

#define T40_WRAP 10240.0f /* tr_scene_t.t40's period: 256 s, 80 blink blocks */

/* Blink clock: a blink (~0.15 s) somewhere in every 3.2 s (128 40 Hz
 * frames), placed by a hash -- the same at 30 and 40 Hz. */
static float blink_open(float t40)
{
	uint32_t n   = (uint32_t)(t40 * (1.0f / 128.0f));
	float    pos = t40 - 128.0f * (float)n, at = (float)(10u + hash(n + 77u) % 100u), d = pos - at - 3.0f;

	d = d < 0.0f ? -d : d;
	return d < 3.0f ? d * (1.0f / 3.0f) : 1.0f;
}

/*
 * The runner's pose for this frame (P3b): the run cycle at the render time
 * tick + phase -- one stride every TR_ANIM_CYCLE ticks, so the stride
 * follows the world (attract's slower world runs a slower cycle) -- with
 * the jump (crouch -> tuck -> reach over the air time, then a landing
 * squash), duck and crash poses blended over it, and a hip roll into a
 * lane change. Blends ease over ~0.1-0.2 s.
 */
static void runner_anim(tr_scene_t *s, const tr_frame_in_t *in, int air, int crash, float ph, float target)
{
	float run[TR_ANIM_CH], jump[TR_ANIM_CH];
	float cyc  = ((float)(in->tick % TR_ANIM_CYCLE) + ph) / (float)TR_ANIM_CYCLE;
	int   duck = (in->flags & TR_FLAG_DUCKING) != 0;
	float k    = frame_k(in); /* the blend rates below are per 40 Hz frame */

	tr_rig_run(cyc, run, 0.0f); /* locked below, over the blended pose */
	uint32_t pf       = s->prev_flags; /* last frame's: the targets before this frame's change */
	int      was_air  = (pf & TR_FLAG_AIRBORNE) != 0, was_crash = (pf & TR_FLAG_CRASH) != 0;
	int      was_duck = (pf & TR_FLAG_DUCKING) != 0;

	ease_k(&s->w_jump, was_air && !was_crash ? 1.0f : 0.0f, air && !crash ? 1.0f : 0.0f, 1.0f / 4.0f, k);
	ease_k(&s->w_duck, was_duck && !was_air && !was_crash ? 1.0f : 0.0f, duck && !air && !crash ? 1.0f : 0.0f,
	       1.0f / 2.0f, k); /* snap into the ball */
	ease_k(&s->w_crash, was_crash ? 1.0f : 0.0f, crash ? 1.0f : 0.0f, 1.0f / 3.0f, k);
	if (was_air && !air && !crash) {
		s->w_land = 1.0f; /* landed one 40 Hz frame ago (ease_k) */
		ease(&s->w_land, 0.0f, 1.0f / 6.0f);
	} else {
		ease(&s->w_land, 0.0f, k / 6.0f);
	}

	/* Jump: crouch at take-off, tucked over the top, reaching for the
	 * ground at the end; the pose holds the air time's end (t = 1, the
	 * reach, set on the landing frame: the same at any frame rate) once it
	 * ends (the jump weight then eases out). */
	if (air || (was_air && !crash)) {
		float t = air ? ((float)(TR_AIR_TICKS - in->air_ticks) + ph) / (float)TR_AIR_TICKS : 1.0f;

		for (int c = 0; c < TR_ANIM_CH; c++) {
			jump[c] = tr_rig_pose(TR_ANIM_POSE_CROUCH)[c];
		}
		mix(jump, tr_rig_pose(TR_ANIM_POSE_TUCK), smooth01(t / 0.35f));
		mix(jump, tr_rig_pose(TR_ANIM_POSE_REACH), smooth01((t - 0.65f) / 0.3f));
		for (int c = 0; c < TR_ANIM_CH; c++) {
			s->jump_ch[c] = jump[c];
		}
	}
	for (int c = 0; c < TR_ANIM_CH; c++) {
		s->ch[c] = run[c];
	}
	mix(s->ch, tr_rig_pose(TR_ANIM_POSE_CROUCH), 0.6f * s->w_land);
	mix(s->ch, s->jump_ch, s->w_jump);
	mix(s->ch, tr_rig_pose(TR_ANIM_POSE_DUCK), s->w_duck);
	mix(s->ch, tr_rig_pose(TR_ANIM_POSE_CRASH), s->w_crash);
	/* P16: the eyes, the idle stance (lobby) and the reaction layer. */
	{
		int       idle = (in->flags & TR_FLAG_IDLE) != 0 && !crash;
		int       was_idle = (pf & TR_FLAG_IDLE) != 0 && !was_crash;
		float     idle_ch[TR_ANIM_CH], spin = 0.0f, iyaw = 0.0f;
		tr_face_t f = {blink_open(s->t40), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

		ease_k(&s->w_idle, was_idle ? 1.0f : 0.0f, idle ? 1.0f : 0.0f, 1.0f / 8.0f, k);
		if (s->w_idle > 0.0f) {
			tr_face_t fi = f;

			iyaw = idle_set((float)in->idle_ms * 0.001f, idle_ch, &fi);
			mix(s->ch, idle_ch, s->w_idle);
			if (idle) {
				f = fi;
			}
		}
		s->w_react = 0.0f;
		if (!crash && !idle) {
			float wr, keep[TR_ANIM_CH];

			memcpy(keep, s->ch, sizeof(keep));
			wr = react_layer(in, s->ch, &f, &spin);
			/* under a duck roll or the crash, the pose they need wins */
			for (int c = 0; c < TR_ANIM_CH; c++) {
				s->ch[c] = keep[c] + (s->ch[c] - keep[c]) * (1.0f - s->w_duck) * (1.0f - s->w_crash);
			}
			s->w_react = wr;
		}
		if (crash) {
			f.open = 1.0f, f.wide = 1.0f, f.look_y = 0.6f; /* hit: eyes wide */
		}
		s->face = f;
		if (idle) {
			s->yaw = iyaw;
		} else if (spin != 0.0f) {
			s->yaw = spin;
		} else {
			/* back to facing the track the short way, ~0.25 s */
			s->yaw = s->yaw > PI_F ? s->yaw - 2.0f * PI_F : s->yaw;
			ease_k(&s->yaw, 0.0f, 0.0f, 0.35f, k);
		}
	}
	/* Lane change: a side hop, leaning into the move (the hips roll; the
	 * instance yaws toward it in the build). The hop rides the move's
	 * progress, 1 -> 0 as runner_x closes on the lane. */
	float hop = 0.0f;
	{
		float dx   = target - s->runner_x;
		float left = clampf((dx < 0.0f ? -dx : dx) / (float)TR_PROJ_LANE_W, 0.0f, 1.0f);

		s->ch[TR_ANIM_P_ROLL] += clampf(dx * 0.08f, -14.0f, 14.0f);
		if (!air && !crash && !duck && left > 0.02f) { /* no hop out of a roll */
			hop = sinf_(PI_F * (1.0f - left));
			s->ch[TR_ANIM_ROOT_Y] += TR_LANE_HOP * hop;
		}
	}
	/* Feet on the board points they touched down on (P3d): the lock over
	 * the blended pose, faded out as the jump / duck / crash poses and the
	 * lane hop take the feet off it (weight 1 alone: tr_rig_run(cyc, .., 1)
	 * bit for bit). */
	tr_rig_foot_lock(cyc, s->ch,
			 (1.0f - s->w_jump) * (1.0f - s->w_duck) * (1.0f - s->w_crash) * (1.0f - hop) * (1.0f - s->w_idle));
	/* Squash and stretch with the bounce: taller in flight, squat at
	 * contact, squashed on landing (scale_y about the feet). */
	s->squash = 1.0f + (1.0f - s->w_duck - s->w_crash > 0.0f ? 1.0f - s->w_duck - s->w_crash : 0.0f) *
				   (1.0f - s->w_idle) * SQUASH * (s->ch[TR_ANIM_ROOT_Y] - tr_anim_run[TR_ANIM_ROOT_Y][0]) -
		    0.08f * s->w_land;
	s->squash = clampf(s->squash, 0.88f, 1.06f);
	/* Duck: curl up (the duck pose blend, ~5 frames), then one forward
	 * roll, done before the duck ends. A crash mid-roll keeps the angle
	 * (crash_roll, -pi..pi) for tr_scene_runner() to ease out. */
	if (crash && !(s->prev_flags & TR_FLAG_CRASH)) {
		float a = s->roll;

		s->crash_roll = a > PI_F ? a - 2.0f * PI_F : a;
	} else if (!crash) {
		s->crash_roll = 0.0f;
	}
	if (duck && !air && !crash) {
		float t = ((float)(TR_DUCK_TICKS - in->duck_ticks) + ph) / (float)TR_DUCK_TICKS;

		s->roll = 2.0f * PI_F * smooth01((t - 0.35f) / 0.55f); /* once curled up */
	} else {
		s->roll = 0.0f;
	}
}

/* The runner's instance as tr_scene_runner() starts it (before a crash,
 * a roll or the board lift move it): lane position, jump lift, squash,
 * the lean into a lane change plus s->yaw. */
static tr_inst_t runner_inst(const tr_scene_t *s, const tr_frame_in_t *in)
{
	float lean = clampf((lane_x(in->lane) - s->runner_x) * 0.003f, -0.45f, 0.45f);

	return (tr_inst_t){NULL, {s->runner_x, s->lift, RUNNER_Z}, lean + s->yaw, s->squash, TR_TRI_GOURAUD, 0.0f};
}

/* Local point p through instance `in`, tr_r3d_emit_mesh()'s expressions. */
static void inst_point(const tr_inst_t *in, const float p[3], float o[3])
{
	float sy, cy, sp = 0.0f, cp = 1.0f, ly = p[1] * in->scale_y, lz = p[2];

	tr_sincosf(in->yaw, &sy, &cy);
	if (in->pitch != 0.0f) {
		float y;

		tr_sincosf(in->pitch, &sp, &cp);
		y  = ly * cp - lz * sp;
		lz = ly * sp + lz * cp;
		ly = y;
	}
	o[0] = p[0] * cy + lz * sy + in->pos.x;
	o[1] = ly + in->pos.y;
	o[2] = -p[0] * sy + lz * cy + in->pos.z;
}

/*
 * The scarf (P16): a verlet chain hanging off the back of the neck. Its
 * anchor and the body collider ride the chest bone through the runner's
 * instance; the chain is stepped at a fixed 120 Hz over the frame's time
 * (3 sub-steps a 40 Hz frame, 4 a 30 Hz one: the same motion at both
 * rates), the anchor interpolated across them. Forces: gravity, the head
 * wind of the run (the world streams by at the game pace; none standing
 * idle or crashed) with a flutter, damping. Constraints: fixed segment
 * lengths from the anchor out, never in front of the neck, and below the
 * back's top corner never in front of the back (the pack) -- the body,
 * roughly. The board needs no constraint here: the simulated neck is never
 * lower than the longest scarf (a roll or a crash lowers only the DRAWN
 * runner; scarf_draw() clamps the strip to the board there, and
 * test_r3d_scene checks the simulated chain stays above it). All IEEE basic ops + tr_sincosf: deterministic.
 */
#define SCARF_HZ    120.0f
#define SCARF_G     1500.0f /* world units / s^2 (~190 units = the runner's height) */
#define SCARF_WIND  2600.0f /* head wind at the play pace */
#define SCARF_DAMP  0.985f  /* velocity kept a sub-step */
#define SCARF_CARRY 0.9f /* of a constraint's move not turned into velocity */
static void scarf_step(tr_scene_t *s, const tr_frame_in_t *in, float k)
{
	const tr_rig_char_t *c = tr_rig_char(s->chr);
	const float         *an = c->anchor[runner_lod(s)];
	const float          bind[2][3] = {{an[0], an[1], an[2]}, {c->scarf[6], c->scarf[7], c->scarf[8]}};
	float                loc[2][3], a[3], col[3], len = c->scarf[4], fwd[3] = {0.0f, 0.0f, 1.0f};
	int                  n = (int)c->scarf[3], nsub = (int)(k * 3.0f + 0.5f);
	tr_inst_t            ri = runner_inst(s, in);
	float                h = 1.0f / SCARF_HZ, wind = 0.0f;
	int                  run = (in->flags & TR_FLAG_ALIVE) && !(in->flags & (TR_FLAG_IDLE | TR_FLAG_CRASH));

	tr_rig_points(s->chr, s->ch, TR_RIG_CHEST, 2, bind, loc);
	inst_point(&ri, loc[0], a);
	inst_point(&ri, loc[1], col);
	if (run) { /* the world's speed: pace_q8 a frame over k 40 Hz frames, 1.0 at the 0.5x play pace */
		wind = SCARF_WIND * (in->pace_q8 ? (float)in->pace_q8 * (1.0f / 128.0f) / k : 2.0f);
	}
	tr_sincosf(ri.yaw, &fwd[0], &fwd[2]); /* the runner's facing: the scarf stays behind the neck */
	if (s->scarf_n != n) { /* lay it out straight back and a little down */
		for (int i = 0; i <= n; i++) {
			s->sc[i][0] = a[0] - 0.94f * len * (float)i * fwd[0], s->sc[i][1] = a[1] - 0.35f * len * (float)i;
			s->sc[i][2] = a[2] - 0.94f * len * (float)i * fwd[2];
			memcpy(s->sc_prev[i], s->sc[i], sizeof(s->sc[i]));
		}
		memcpy(s->sc_anchor, a, sizeof(a));
		s->scarf_n = (uint8_t)n;
	}
	nsub = nsub < 1 ? 1 : nsub;
	for (int j = 1; j <= nsub; j++) {
		float u = (float)j / (float)nsub, t = (s->t40 + k * u) * (1.0f / 40.0f);

		for (int q = 0; q < 3; q++) {
			s->sc[0][q] = s->sc_anchor[q] + (a[q] - s->sc_anchor[q]) * u;
		}
		for (int i = 1; i <= n; i++) {
			float *p = s->sc[i], *o = s->sc_prev[i], fy = sinf_(t * 23.0f + (float)i * 1.3f);
			float  acc[3] = {140.0f * sinf_(t * 17.0f + (float)i * 0.9f) * (wind * (1.0f / SCARF_WIND)),
					 -SCARF_G + 520.0f * fy * (wind * (1.0f / SCARF_WIND)), -wind};

			for (int q = 0; q < 3; q++) {
				float v = (p[q] - o[q]) * SCARF_DAMP;

				o[q] = p[q];
				p[q] += v + acc[q] * h * h;
			}
		}
		for (int i = 1; i <= n; i++) {
			float *p = s->sc[i], *o = s->sc_prev[i], was[3] = {p[0], p[1], p[2]}, d[3], m;

			m = (p[0] - s->sc[0][0]) * fwd[0] + (p[2] - s->sc[0][2]) * fwd[2];
			if (m > 0.0f) { /* never in front of the neck: over the shoulder is off limits */
				p[0] -= m * fwd[0], p[2] -= m * fwd[2];
			}
			m = (p[0] - col[0]) * fwd[0] + (p[2] - col[2]) * fwd[2];
			if (p[1] < col[1] && m > 0.0f) { /* below the back's top: behind the back / pack */
				p[0] -= m * fwd[0], p[2] -= m * fwd[2];
			}
			m = 0.0f;
			for (int q = 0; q < 3; q++) {
				d[q] = p[q] - s->sc[i - 1][q], m += d[q] * d[q];
			}
			m = m > 1e-6f ? len / __builtin_sqrtf(m) : 1.0f;
			for (int q = 0; q < 3; q++) {
				p[q] = s->sc[i - 1][q] + d[q] * m;
			}
			/* the constraints move the point, they give it no speed
			 * (follow-the-leader otherwise pumps energy in) */
			for (int q = 0; q < 3; q++) {
				o[q] += SCARF_CARRY * (p[q] - was[q]);
			}
		}
	}
	memcpy(s->sc_anchor, a, sizeof(a));
}

void tr_scene_step(tr_scene_t *s, const tr_frame_in_t *in)
{
	float target = lane_x(in->lane);
	int   alive  = (in->flags & TR_FLAG_ALIVE) != 0;
	int   air    = (in->flags & TR_FLAG_AIRBORNE) != 0;
	int   crash  = (in->flags & TR_FLAG_CRASH) != 0;
	float ph     = phase_f(in);
	/* Particle time step: crash slow motion, else one frame. */
	float dt = ((crash && in->crash_tick <= SLOWMO_FRAMES) ? SLOWMO_RATE : 1.0f) * pace_f(in); /* at the game pace */
	float k  = frame_k(in);

	/* Exponential slides, 35 % / 18 % of the way a 40 Hz frame (ln 0.65,
	 * ln 0.82), the lane change one 40 Hz frame old (ease_k). */
	float was = lane_x(s->prev_lane);

	slide_k(&s->runner_x, was, target, 0.35f, -0.43078292f, k);
	slide_k(&s->cam_x, was, target, 0.18f, -0.19845094f, k);
	/* Bank into the lane change, proportional to how far the camera still
	 * has to travel (one lane = full bank). */
	s->cam_roll = clampf((s->cam_x - target) / (float)TR_PROJ_LANE_W, -1.0f, 1.0f) * TR_CAM_BANK_DEG * DEG;

	/* air_ticks counts TR_AIR_TICKS-1 .. 1 while airborne: a half-sine arc.
	 * Frozen through a crash: the build blends it out from where it was. */
	if (!crash) {
		s->lift = air ? JUMP_H * sinf_(PI_F * ((float)(TR_AIR_TICKS - in->air_ticks) + ph) / (float)TR_AIR_TICKS)
			      : 0.0f;
	}
	/* Landing dip: -24, -16, -8 over three 40 Hz frames from the landing
	 * frame -- a clock of 40 Hz frames left, stepped by k. */
	if ((s->prev_flags & TR_FLAG_AIRBORNE) && !air) {
		s->dip_t = 3.0f; /* landed one 40 Hz frame ago (ease_k) */
	} else {
		s->dip_t = s->dip_t > k ? s->dip_t - k : 0.0f;
	}
	s->dip = -8.0f * s->dip_t;
	s->chr = (uint8_t)char_of(in);
	runner_anim(s, in, air, crash, ph, target);
	/* The camera rides the runner's hip bob (two a stride), mean removed. */
	s->cam_bob = (alive && !air) ? 0.7f * (s->ch[TR_ANIM_ROOT_Y] - tr_anim_run[TR_ANIM_ROOT_Y][0]) * (1.0f - s->w_idle)
				     : 0.0f;
	scarf_step(s, in, k);
	/* wrapped every 256 s (a whole number of blink blocks): the scarf's
	 * flutter phases stay inside tr_sincosf's exact range */
	s->t40 += k;
	s->t40 = s->t40 >= T40_WRAP ? s->t40 - T40_WRAP : s->t40;

	for (int i = 0; i < TR_PARTICLES; i++) {
		tr_particle_t *p = &s->p[i];

		if (p->life > 0.0f) {
			/* ballistic, exact for any dt (gravity 1.4 a frame^2): the
			 * same arc at 30 and 40 Hz */
			p->pos.x += p->vel.x * dt, p->pos.y += (p->vel.y - 0.7f * dt) * dt, p->pos.z += p->vel.z * dt;
			p->vel.y -= 1.4f * dt;
			p->life -= dt;
		}
	}
	/* A pickup (step.c: +10) -- judged only against a frame that was
	 * already alive, so the first frame or a restart never bursts. */
	if (alive && (s->prev_flags & TR_FLAG_ALIVE) && in->score >= s->prev_score + 10u) {
		for (int i = 0; i < TR_PARTICLES; i++) {
			uint32_t r = xorshift(&s->rng);

			/* From the chest, sprayed up and out toward the camera side
			 * so the runner's own body does not hide the burst. */
			s->p[i].pos  = (tr_v3_t){s->runner_x, s->lift + 130.0f, RUNNER_Z - 30.0f};
			s->p[i].vel  = (tr_v3_t){(float)(int32_t)(r & 31u) - 15.5f, 8.0f + (float)((r >> 5) & 15u),
						 -(float)((r >> 9) & 7u)};
			s->p[i].life  = (float)(16u + ((r >> 13) & 7u));
			s->p[i].col   = (i & 1) ? 5 : 7; /* COPPER_LIT / SOLDER_LIT */
			s->p[i].spark = 0;
		}
	}
	/* The hit: copper and solder shards off the obstacle's struck face,
	 * at shin height for a low part, head height for a high one, thrown up
	 * and back toward the camera past the runner. Once per crash, on its
	 * first frame seen (the A32 may skip crash_tick 0). */
	if (crash && !(s->prev_flags & TR_FLAG_CRASH)) {
		static const uint8_t col[2][4] = {{5, 7, 4, 7},    /* COPPER_LIT SOLDER_LIT COPPER SOLDER_LIT */
						  {7, 12, 7, 11}}; /* a live wire: white, light blue, blue */
		int                  wire   = in->crash_kind == TR_CRASH_KIND_WIRE;
		int                  low    = wire ? in->ents[in->crash_ent & 15u].low : in->crash_kind == TR_CRASH_KIND_LOW;
		float                hy     = low ? 50.0f : 150.0f;

		for (int i = 0; i < TR_PARTICLES; i++) {
			uint32_t r = xorshift(&s->rng);

			/* Spread wider than the runner so the burst shows on the
			 * very first frame, from behind its body. */
			s->p[i].pos  = (tr_v3_t){lane_x(in->crash_lane) + (float)(int32_t)((r >> 20) & 127u) - 63.5f,
						 hy + (float)(int32_t)((r >> 27) & 31u) - 15.5f, RUNNER_Z + CONTACT_DZ - 30.0f};
			s->p[i].vel   = (tr_v3_t){(float)(int32_t)(r & 63u) - 31.5f, 6.0f + (float)((r >> 6) & 31u),
						  -1.0f - (float)((r >> 11) & 3u)};
			s->p[i].life  = (float)(16u + ((r >> 15) & 15u));
			s->p[i].col   = col[wire][i & 3];
			s->p[i].spark = 1;
		}
	}
	s->prev_score = in->score;
	s->prev_flags = in->flags;
	s->prev_lane  = in->lane;
}

static void emit_f(tr_dl_t *dl, const tr_cam_t *c, const tr_light_t *l, const tr_mesh_t *m, float x, float y,
		   float z, float yaw, float sy, uint8_t flags)
{
	tr_inst_t in = {m, {x, y, z}, yaw, sy, flags, 0.0f};

	tr_r3d_emit_mesh(dl, c, l, &in);
}

static void emit(tr_dl_t *dl, const tr_cam_t *c, const tr_light_t *l, const tr_mesh_t *m, float x, float y,
		 float z, float yaw, float sy)
{
	emit_f(dl, c, l, m, x, y, z, yaw, sy, TR_TRI_GOURAUD);
}

/* Lowest world y of mesh `m` placed by `in` (its own mesh ignored) --
 * scale_y and pitch applied with the expressions tr_r3d_emit_mesh() uses. */
static float lowest_y(const tr_inst_t *in, const tr_mesh_t *m)
{
	float sp = 0.0f, cp = 1.0f, lo = 0.0f;

	if (in->pitch != 0.0f) {
		tr_sincosf(in->pitch, &sp, &cp);
	}
	for (uint16_t i = 0; i < m->nv; i++) {
		float ly = (m->vf ? m->vf[i * 3 + 1] : (float)m->v[i * 3 + 1]) * in->scale_y;
		float lz = m->vf ? m->vf[i * 3 + 2] : (float)m->v[i * 3 + 2];
		float y  = (in->pitch != 0.0f ? ly * cp - lz * sp : ly) + in->pos.y;

		lo = y < lo ? y : lo;
	}
	return lo;
}

/* Raises `in` so its lowest vertex -- over in->mesh and, when not NULL,
 * `also` (a second mesh drawn with the same instance: the runner's limbs)
 * -- sits on the board (y = 0). The ground is a NOZ background (r3d.h
 * TR_TRI_NOZ) and hides nothing, so a tipped-over runner or obstacle must
 * not reach under it. Meshes above the board are left untouched. Returns
 * the lift (>= 0). */
static float on_board(tr_inst_t *in, const tr_mesh_t *also, int n_also)
{
	float lo = lowest_y(in, in->mesh);

	for (int i = 0; i < n_also; i++) {
		float l2 = lowest_y(in, &also[i]);

		lo = l2 < lo ? l2 : lo;
	}
	in->pos.y -= lo;
	return -lo;
}

/* Camera-space depth of ground point (x, 0, z). */
static float view_z(const tr_cam_t *cam, float x, float z)
{
	return cam->view.m[2][0] * x + cam->view.m[2][2] * z + cam->view.m[2][3];
}

/*
 * Near board beyond the shoulders (|x| 840..2520), TR_TRI_NOZ: one flat quad
 * a side from zs while no corner reaches fog (view z <= FOG_START, where the
 * board mesh shades to palette 2, solder mask, exactly), then the per-tile
 * Gouraud board mesh, fogged per vertex as before, for tiles k0.. up to nt
 * (the last tile or two before TEX_Z). zb: tile 0's near edge.
 *
 * Its edge and the shoulder runs' edge at x = +-840 have different vertices
 * (a T-junction: the runs split at NEAR_SPLIT, the board at the fog tiles),
 * so the two rasterised lines disagree by a pixel here and there and the
 * background sparkled through. The board therefore reaches BOARD_UNDER in
 * under the shoulders and is emitted before them: NOZ triangles draw in DL
 * order, so the texture covers the overlap and the seam is interior.
 */
#define BOARD_UNDER 24.0f /* world units: >= 4 px even at TEX_Z */
static const uint16_t *pal_of(const tr_light_t *l)
{
	return l->pal ? l->pal : tr_r3d_palette;
}

static void board_near(tr_dl_t *dl, const tr_cam_t *cam, const zctx_t *zc, int32_t zb, float zs, uint32_t k0,
		       uint32_t nt)
{
	const float       xo = 10.5f * (float)TR_PROJ_LANE_W, xi = 3.5f * (float)TR_PROJ_LANE_W - BOARD_UNDER;
	uint32_t          k  = k0;
	float             zt = (float)(zb + (int32_t)nt * TR_TILE_LEN) - 0.5f * (float)TR_TILE_LEN;
	const tr_light_t *lg = &zc->lg[beyond(zc, zt)];

	/* Under everything at the TEX_Z edge: a board mesh straddling it, drawn
	 * first, so the far tiles' different vertices there leave no gap. */
	emit_f(dl, cam, lg, &tr_mesh_tile_board, -xo + BOARD_UNDER, 0, zt, 0, 1.0f, TR_TRI_GOURAUD | TR_TRI_NOZ);
	emit_f(dl, cam, lg, &tr_mesh_tile_board, xi, 0, zt, 0, 1.0f, TR_TRI_GOURAUD | TR_TRI_NOZ);
	while (k < nt) {
		float z1 = (float)(zb + (int32_t)(k + 1) * TR_TILE_LEN);

		if (view_z(cam, -xo, z1) > FOG_START || view_z(cam, xo, z1) > FOG_START) {
			break;
		}
		k++;
	}
	if (k > k0) {
		float ze = (float)(zb + (int32_t)k * TR_TILE_LEN);

		/* split at the gate: each zone's own board colour (MASK) */
		float zg = zc->gate_z > zs && zc->gate_z < ze ? zc->gate_z : ze;

		for (int part = 0; part < 2; part++) {
			float a = part ? zg : zs, b = part ? ze : zg;

			if (b <= a) {
				continue;
			}
			uint16_t c = pal_of(&zc->lg[beyond(zc, a)])[2];

			for (int side = -1; side <= 1; side += 2) {
				/* same x as the tile meshes below: shared corners at ze */
				float   x0   = side < 0 ? -xo + BOARD_UNDER : xi, x1 = x0 + 7.0f * (float)TR_PROJ_LANE_W;
				tr_v3_t q[4] = {{x0, 0, a}, {x0, 0, b}, {x1, 0, b}, {x1, 0, a}};

				tr_r3d_emit_quad(dl, cam, q, c, TR_TRI_NOZ);
			}
		}
	}
	for (; k < nt; k++) {
		float z0 = (float)(zb + (int32_t)k * TR_TILE_LEN);

		lg = &zc->lg[beyond(zc, z0 + 0.5f * (float)TR_TILE_LEN)];
		emit_f(dl, cam, lg, &tr_mesh_tile_board, -xo + BOARD_UNDER, 0, z0, 0, 1.0f, TR_TRI_GOURAUD | TR_TRI_NOZ);
		emit_f(dl, cam, lg, &tr_mesh_tile_board, xi, 0, z0, 0, 1.0f, TR_TRI_GOURAUD | TR_TRI_NOZ);
	}
}

/* The eye's world z, and the textured ground's start grid (1/16 tile: uv
 * 0x100 a step at TR_TRI_UVX8 scale, so every corner's uv is exact). */
#define EYE_Z      ((int32_t)TR_PROJ_Z_RUNNER - (int32_t)TR_CAM_BACK)
#define NEAR_GRID  (TR_TILE_LEN / 16)
#define NEAR_SPLIT 5 /* tile boundary between the two textured runs, counted from zb */
_Static_assert((int32_t)TEX_Z / TR_TILE_LEN + 4 < 16, "near ground uv must fit 16 repeats (TR_TRI_UVX8)");
_Static_assert(NEAR_SPLIT * TR_TILE_LEN > 3 * TR_TILE_LEN + TR_PROJ_Z_RUNNER - (int32_t)TR_CAM_BACK &&
		       NEAR_SPLIT * TR_TILE_LEN < 2 * TR_TILE_LEN + (int32_t)TEX_Z,
	       "NEAR_SPLIT must fall inside the textured run for every scroll phase");

/*
 * Near textured ground from zs to ze, TR_TRI_NOZ (the background layer): the
 * 3 lanes and the 2 shoulder columns a side. One long quad per texture run
 * -- shoulders, lanes, shoulders -- instead of a quad per column per tile:
 * the texture repeats by u/v wrap (TR_TRI_UVX8, 0x1000 a repeat), so a call
 * sets up 6 triangles where a tile took 14, and a row is 3 spans, not 7 x 2.
 * The first zs is the grid line at or just behind the eye: the ground under
 * the camera is below the screen's bottom edge (view z >= ~190) yet in front
 * of the near plane (view z >= ~65 there), so these quads are never
 * near-clipped and every corner has its exact uv (a clip vertex would round
 * its uv, and move with the scroll). v0/v1: the zs/ze v.
 */
static void ground_run(tr_dl_t *dl, const tr_cam_t *cam, float zs, float ze, uint16_t v0, uint16_t v1, uint8_t slot0)
{
	static const struct {
		float   x0; /* lane widths from the centre */
		uint8_t cols, tex;
	} run[3] = {{-3.5f, 2, TR_SCENE_TEX_BOARD}, {-1.5f, 3, TR_SCENE_TEX_LANE}, {1.5f, 2, TR_SCENE_TEX_BOARD}};
	const float lw = (float)TR_PROJ_LANE_W;

	for (int r = 0; r < 3; r++) {
		float          x0       = run[r].x0 * lw, x1 = x0 + (float)run[r].cols * lw;
		uint16_t       u1       = (uint16_t)(run[r].cols * 0x1000u);
		tr_v3_t        q[4]     = {{x0, 0, zs}, {x0, 0, ze}, {x1, 0, ze}, {x1, 0, zs}};
		const uint16_t uv[4][2] = {{0, v0}, {0, v1}, {u1, v1}, {u1, v0}};

		tr_r3d_emit_quad_tex(dl, cam, q, uv, (uint8_t)(run[r].tex + slot0), TR_TRI_NOZ | TR_TRI_UVX8);
	}
}

/* ground_run() over [zs, ze] (both on the NEAR_GRID lines from zb, v
 * 0x100 a line), split at the gate's grid line nearest gate_z: the zone
 * beyond it through the _FAR slots. The seam moves in NEAR_GRID steps under
 * the gate's threshold bar (GATE_BAR, twice as deep). */
static void ground_near(tr_dl_t *dl, const tr_cam_t *cam, const zctx_t *zc, int32_t zb, float zs, float ze, uint16_t v0,
			uint16_t v1)
{
	float g = zc->gate_z;

	if (zc->near != zc->far && g > zs && g < ze) {
		int32_t gi = (int32_t)((g - (float)zb) / (float)NEAR_GRID + 0.5f);
		float   zg = (float)(zb + gi * NEAR_GRID);

		if (zg > zs && zg < ze) {
			uint16_t vg = (uint16_t)(v0 + (uint32_t)((zg - zs) / (float)NEAR_GRID + 0.5f) * 0x100u);

			ground_run(dl, cam, zs, zg, v0, vg, TR_SCENE_TEX_LANE);
			ground_run(dl, cam, zg, ze, vg, v1, TR_SCENE_TEX_LANE_FAR);
			return;
		}
	}
	ground_run(dl, cam, zs, ze, v0, v1, zc->near != zc->far && g <= zs ? TR_SCENE_TEX_LANE_FAR : TR_SCENE_TEX_LANE);
}

/* A ground mesh (flat, y 0, z 0..its own depth) at (x, z0) stretched to
 * `len` deep, Gouraud-fogged per vertex. */
static void stretch(tr_dl_t *dl, const tr_cam_t *cam, const tr_light_t *lg, const tr_mesh_t *m, float x, float z0,
		    float len)
{
	float     v[16 * 3];
	tr_mesh_t cut = *m;

	for (int i = 0; i < m->nv * 3 && i < 16 * 3; i++) {
		v[i] = i % 3 == 2 && m->v[i] > 0 ? len : (float)m->v[i];
	}
	cut.vf = v;
	emit_f(dl, cam, lg, &cut, x, 0, z0, 0, 1.0f, TR_TRI_GOURAUD | TR_TRI_NOZ);
}

/* The far road, zs (the near ground's far edge) to GROUND_END: the lanes and
 * shoulders textured as near, through the averaged slots (avg_tex(): the
 * texture minified, steady as the board scrolls; past a gate the incoming
 * zone's), fogged per sub-span like the near ground;
 * the plain board outside them in a few depth pieces (Gouraud fog, linear
 * over each). ~30 triangles for the whole far third of the screen. */
static const float far_cut[] = {4500.0f, 7000.0f, 11000.0f, 16000.0f};
static void far_road(tr_dl_t *dl, const tr_cam_t *cam, const zctx_t *zc, float zs)
{
	const float xo = 10.5f * (float)TR_PROJ_LANE_W, xi = 3.5f * (float)TR_PROJ_LANE_W - BOARD_UNDER;
	float       a  = zs;

	for (unsigned k = 0; k <= sizeof(far_cut) / sizeof(far_cut[0]); k++) { /* the board outside the lanes */
		float b = k < sizeof(far_cut) / sizeof(far_cut[0]) ? far_cut[k] : GROUND_END;

		if (b > a) {
			const tr_light_t *lg = &zc->lg[beyond(zc, 0.5f * (a + b))];

			stretch(dl, cam, lg, &tr_mesh_tile_board, -xo + BOARD_UNDER, a, b - a);
			stretch(dl, cam, lg, &tr_mesh_tile_board, xi, a, b - a);
			a = b;
		}
	}
	/* the camera-side zone to the gate, the incoming one past it */
	float zg = zc->near == zc->far ? GROUND_END : zc->gate_z < zs ? zs : zc->gate_z > GROUND_END ? GROUND_END : zc->gate_z;

	for (uint32_t h = 0; h < 2u; h++) {
		float hs = h ? zg : zs, he = h ? GROUND_END : zg;

		for (uint32_t k = 0; k < AVG_LEVELS; k++) { /* a level a depth range (avg_tex()) */
			float a0 = k ? avg_z[k - 1] : hs, b0 = k + 1 < AVG_LEVELS ? avg_z[k] : he;

			a0 = a0 > hs ? a0 : hs, b0 = b0 < he ? b0 : he;
			if (b0 > a0) {
				ground_run(dl, cam, a0, b0, avg_v(k, h), avg_v(k, h),
					   (uint8_t)(h ? TR_SCENE_TEX_LANE_AVG_FAR : TR_SCENE_TEX_LANE_AVG));
			}
		}
	}
}

/* emit() for a shoulder part standing at (x, 0, z): skipped when a sphere
 * of radius tr_scene_wall_r around (x, 100, z) projects wholly left or right
 * of the screen -- the near tiles' parts are mostly out of the frame, and an
 * off-screen triangle still costs its transform and raster setup. */
float tr_scene_wall_r = TR_WALL_R;

/* The palette entry of the board itself (tr_mesh_tile_far's col 2): what a
 * far part stands on. */
#define BOARD_IDX 2

/* A sphere of radius R round c projects wholly left or right of the screen. */
static int off_screen(const tr_cam_t *cam, tr_v3_t c, float R)
{
	tr_sv_t p;
	float   vz;

	if (tr_r3d_project(cam, c, &p, &vz) && vz > R) {
		float r = R * cam->f_px / vz, px = (float)p.x / (float)(1 << TR_R3D_SUB);

		return px + r < 0.0f || px - r > (float)TR_R3D_W;
	}
	return 0;
}

/* Far scenery (past tr_scene_wall_bb_z): a part is a few px, so it goes in as a
 * billboard -- one quad facing the camera, the mesh's width and height
 * (sy scaled), its colours' mean at an average light, fogged like the part
 * -- 2 triangles where its low-poly mesh took 6-24, out to the skyline. */
static uint16_t glow_fog(const tr_light_t *l, uint16_t c, float vz, float fk);
#define FAR_BB_MIN_PX 3.0f
static void far_bb(tr_dl_t *dl, const tr_cam_t *cam, const tr_light_t *l, const tr_mesh_t *m, float x, float z, float sy)
{
	const uint16_t *pal = m->pal ? m->pal : l->pal ? l->pal : tr_r3d_palette;
	float           hw = 0.0f, h = 0.0f, rgb[3] = {0.0f, 0.0f, 0.0f};
	float           in = l->ambient + (1.0f - l->ambient) * 0.55f; /* a lit side's average */

	for (int i = 0; i < m->nv; i++) { /* any yaw: the larger of x and z */
		float ax = (float)(m->v[i * 3] < 0 ? -m->v[i * 3] : m->v[i * 3]), az = (float)(m->v[i * 3 + 2] < 0 ? -m->v[i * 3 + 2] : m->v[i * 3 + 2]);

		hw = ax > hw ? ax : hw, hw = az > hw ? az : hw;
		h  = (float)m->v[i * 3 + 1] > h ? (float)m->v[i * 3 + 1] : h;
	}
	float area = 0.0f; /* the mesh's silhouette: its faces' area seen from the front, the far half */

	for (int t = 0; t < m->nt; t++) {
		uint16_t       c = pal[m->col[t] & 15u];
		const int16_t *a = &m->v[m->tri[t * 3] * 3], *b = &m->v[m->tri[t * 3 + 1] * 3], *d = &m->v[m->tri[t * 3 + 2] * 3];
		float          cr = (float)(b[0] - a[0]) * (float)(d[1] - a[1]) - (float)(b[1] - a[1]) * (float)(d[0] - a[0]);

		area += 0.5f * (cr < 0.0f ? -cr : cr); /* a low mesh has no back faces: its front only */
		rgb[0] += (float)(c >> 11), rgb[1] += (float)((c >> 5) & 63u), rgb[2] += (float)(c & 31u);
	}
	/* a dish on its stand fills little of its box: the quad as wide as
	 * keeps the part's area, so it neither swells nor thins as it switches
	 * (test_scene_load.c: a tile's switch steps <= ~110 px, most ~10-50) */
	float fill = h > 0.0f && hw > 0.0f ? area / (2.0f * hw * h) : 1.0f;

	hw *= fill < 0.3f ? 0.3f : fill > 1.0f ? 1.0f : fill;
	float vz = view_z(cam, x, z);

	if (h * sy * cam->f_px < FAR_BB_MIN_PX * vz) {
		return; /* under FAR_BB_MIN_PX tall there: a fogged speck the horizon band's bin can spare */
	}
	float    k = in / (float)(m->nt ? m->nt : 1);
	uint16_t c = (uint16_t)((uint32_t)(rgb[0] * k) << 11 | (uint32_t)(rgb[1] * k) << 5 | (uint32_t)(rgb[2] * k));
	tr_v3_t  q[4] = {{x + hw, 0.0f, z}, {x - hw, 0.0f, z}, {x - hw, h * sy, z}, {x + hw, h * sy, z}};

	tr_r3d_emit_quad(dl, cam, q, glow_fog(l, c, vz, 1.0f), 0);
}

/* Past tr_scene_wall_bb_z a part is a billboard (far_bb()). */
static int as_bb(float z)
{
	return z > tr_scene_wall_bb_z;
}

static void wall(tr_dl_t *dl, const tr_cam_t *cam, const tr_light_t *l, const tr_mesh_t *m, float x, float z,
		 float yaw, float sy)
{
	if (as_bb(z)) {
		far_bb(dl, cam, l, m, x, z, sy);
	} else if (!off_screen(cam, (tr_v3_t){x, 100.0f, z}, tr_scene_wall_r)) {
		emit(dl, cam, l, m, x, 0, z, yaw, sy);
	}
}

/* Emissive colour c at camera depth vz: fk x the ground's fog (1 = fogged
 * like the board it lies on; less reads further through the haze). */
static uint16_t glow_fog(const tr_light_t *l, uint16_t c, float vz, float fk)
{
	uint32_t a = (uint32_t)(256.0f * fk * tr_r3d_fog_amount(l, vz));

	return a ? mix565(c, RGB565(l->fog[0], l->fog[1], l->fog[2]), a > 256u ? 256u : a) : c;
}

/* sqrt(x), x >= 0, from IEEE basic ops (host == A32 bits, no libm): the
 * bit-trick 1/sqrt guess and three Newton steps, as seg() does. */
static float sqrt_(float x)
{
	uint32_t bits;
	float    inv;

	if (x <= 0.0f) {
		return 0.0f;
	}
	memcpy(&bits, &x, 4);
	bits = 0x5F3759DFu - (bits >> 1);
	memcpy(&inv, &bits, 4);
	for (int k = 0; k < 3; k++) {
		inv = inv * (1.5f - 0.5f * x * inv * inv);
	}
	return x * inv;
}

/* A copy of l drawing a whole piece (the gate) with one fog: l's own
 * haze at view depth vz, then faded by f (0..1) toward the board's colour
 * there (lg's, fogged the same) -- a far piece melts into the road it
 * stands on rather than into a glow the far road no longer is (FOG_KNEE).
 * The span trick of ent_fog(): amount a = 1 - (1 - haze)(1 - f) over the
 * whole piece, toward the colour haze-then-fade leaves. */
static tr_light_t fade_light(const tr_light_t *l, const tr_light_t *lg, float vz, float f)
{
	tr_light_t o    = *l;
	float      w    = tr_r3d_fog_amount(l, vz), a = 1.0f - (1.0f - w) * (1.0f - f);
	uint16_t   road = glow_fog(lg, pal_of(lg)[BOARD_IDX], vz, 1.0f);
	uint32_t   rd[3] = {(uint32_t)(road >> 11) << 3, (uint32_t)((road >> 5) & 63u) << 2, (uint32_t)(road & 31u) << 3};

	o.fog_far   = 0.0f;
	o.fog_start = vz - (1.0f - sqrt_(1.0f - a)) * ENT_FOG_SPAN;
	o.fog_end   = o.fog_start + ENT_FOG_SPAN;
	for (int c = 0; a > 0.0f && c < 3; c++) {
		o.fog[c] = (uint8_t)(((float)l->fog[c] * w * (1.0f - f) + (float)rd[c] * f) / a + 0.5f);
	}
	return o;
}

/* ------------------------------------------------------ living board (P12)
 * Time for everything below: time_q4() (above). */

/* Shoulder deco a tile side carries (tr_scene_deco): a fan on 3 in 4 QFPs
 * and BGAs nearer than LOD_Z, and SMD LEDs nearer than LED_Z -- none, one
 * blinking, or a row of three chasing -- on the board in the gap before the
 * tile's parts (every part keeps behind z0 + 50), running outward. */
#define LED_Z   3600.0f
#define LED_Z0  300.0f /* nearer, a lit glow at the screen's side edge is a big, mostly unseen fill */
#define LED_X   420.0f
#define LED_DX  34.0f
#define LED_R   56.0f /* the glow's half width on the board */
static const uint16_t led_rgb[4] = {RGB565(255, 40, 32), RGB565(40, 255, 80), RGB565(60, 150, 255),
				    RGB565(255, 176, 24)};

void tr_scene_deco(uint32_t tile, int side, float z0, uint8_t q, const tr_frame_in_t *in, tr_deco_t *d)
{
	float lod_z = (q & TR_LOD_NEAR) ? TR_LOD_NEAR_Z : LOD_Z; /* walls()' low-poly threshold */

	uint32_t h  = hash(tile * 2u + (side > 0)); /* walls()' part choice */
	uint32_t hh = hash(h ^ 0x1ED5EEDu), tq = time_q4(in) + ((hh >> 8) & 1023u);
	float    sx = (float)side, zc = z0 + (float)TR_TILE_LEN * 0.5f;

	*d   = (tr_deco_t){0};
	d->h = h;
	if (((h & 7u) == 3u || (h & 7u) == 4u) && ((h >> 22) & 3u) != 0u && z0 <= lod_z) {
		/* 0.6 rad a tick (a 5-blade rotor reads as spinning, not strobing,
		 * up to ~1 tick a frame), either way round; Q16 turns. */
		uint32_t turn = (tq * 391u) & 0xFFFFu;

		d->fan       = (tr_v3_t){sx * ((h & 7u) == 3u ? 510.0f : 500.0f), (h & 7u) == 3u ? 30.0f : 44.0f, zc};
		d->frame_yaw = (float)((int32_t)((h >> 8) & 7u) - 4) * 0.06f; /* walls()' twist */
		d->fan_yaw   = (float)turn * (2.0f * PI_F / 65536.0f) * ((hh >> 20) & 1u ? 1.0f : -1.0f);
	}
	if ((hh & 3u) != 0u && z0 < LED_Z && z0 > LED_Z0) {
		d->n_led   = (hh & 3u) == 3u ? 3 : 1;
		d->led_col = (uint8_t)((hh >> 2) & 3u);
		d->led     = (tr_v3_t){sx * LED_X, 0.0f, z0 + 24.0f};
		d->led_lo  = z0 > lod_z;
		if (d->n_led == 1) { /* blink: 0.5 .. 1.2 ticks x 16 period, half on */
			uint32_t per = (10u + ((hh >> 4) & 7u) * 2u) * 16u;

			d->led_on = (uint8_t)(tq % per < per / 2u);
		} else { /* chase: one lit, 3 ticks each, then a dark beat */
			uint32_t k = (tq / 48u) & 3u;

			d->led_on = (uint8_t)(k < 3u ? 1u << k : 0u);
		}
	}
}

/* One SMD LED at (x, 0, z): its top and camera face (a 20 x 12 x 14 box),
 * lit or dark; lit, a glow diamond on the board round it (bright centre,
 * board-coloured rim). Far off (lo) the body is its top only. */
static void led(tr_dl_t *dl, const tr_cam_t *cam, const tr_light_t *l, float x, float z, uint8_t col, int on, int lo)
{
	float    vz   = view_z(cam, x, z);
	uint16_t c    = on ? led_rgb[col] : mix565(led_rgb[col], RGB565(24, 16, 16), 190);
	uint16_t top  = glow_fog(l, on ? mix565(c, RGB565(255, 255, 255), 120) : c, vz, on ? 0.4f : 1.0f);
	tr_v3_t  qt[4] = {{x - 10, 12, z - 7}, {x - 10, 12, z + 7}, {x + 10, 12, z + 7}, {x + 10, 12, z - 7}};

	tr_r3d_emit_quad(dl, cam, qt, top, 0);
	if (!lo) {
		tr_v3_t qf[4] = {{x - 10, 0, z - 7}, {x - 10, 12, z - 7}, {x + 10, 12, z - 7}, {x + 10, 0, z - 7}};

		tr_r3d_emit_quad(dl, cam, qf, glow_fog(l, c, vz, on ? 0.4f : 1.0f), 0);
	}
	if (on) {
		uint16_t rim = glow_fog(l, RGB565(14, 41, 33), vz, 1.0f); /* tex_board's mask, as fogged */
		uint16_t ctr = glow_fog(l, mix565(RGB565(14, 41, 33), led_rgb[col], 230), vz, 0.5f);
		tr_v3_t  cc = {x, 0.4f, z}, lf = {x - LED_R, 0.4f, z}, r = {x + LED_R, 0.4f, z};
		tr_v3_t  f = {x, 0.4f, z + 2.0f * LED_R}, n = {x, 0.4f, z - 2.0f * LED_R};
		tr_v3_t  far[4] = {cc, lf, f, r}, nr[4] = {cc, r, n, lf};
		uint16_t cf[4] = {ctr, rim, rim, rim};

		tr_r3d_emit_quad_rgb(dl, cam, far, cf, 0);
		tr_r3d_emit_quad_rgb(dl, cam, nr, cf, 0);
	}
}

/* A tile side's deco d (tr_scene_deco), after its parts. */
static void deco(tr_dl_t *dl, const tr_cam_t *cam, const tr_light_t *l, const tr_deco_t *dp, int side)
{
	const tr_deco_t d = *dp;

	/* Culled like wall() (a sphere wholly off screen), radii scaled from
	 * tr_scene_wall_r (400: 100 bounds a fan, 180 an LED row with its glow,
	 * both with the same ~20 % to spare), so the test hook turns them off too. */
	if (d.fan.y > 0.0f && !off_screen(cam, d.fan, 0.25f * tr_scene_wall_r)) {
		emit(dl, cam, l, &tr_mesh_fan_frame, d.fan.x, d.fan.y, d.fan.z, d.frame_yaw, 1.0f);
		emit(dl, cam, l, &tr_mesh_fan, d.fan.x, d.fan.y, d.fan.z, d.fan_yaw, 1.0f);
	}
	if (d.n_led && off_screen(cam, (tr_v3_t){d.led.x + (float)side * LED_DX, 0.0f, d.led.z}, 0.45f * tr_scene_wall_r)) {
		return;
	}
	for (int k = 0; k < d.n_led; k++) {
		led(dl, cam, l, d.led.x + (float)side * (float)k * LED_DX, d.led.z, d.led_col, (d.led_on >> k) & 1, d.led_lo);
	}
}

/* Both walls for world tile `idx` at depth z0, chosen by the tile's hash: an
 * inner rank on the shoulder -- electrolytic caps, a DIP, a QFP, a BGA, a
 * power inductor with a crystal -- and a taller back rank of big caps; near
 * the camera, small parts at the lane edge too (0603s + a SOT-23, a raised
 * jumper wire), too small to draw far. Tall parts span many bands and count
 * in every band's bin (TR_BIN_MAX), hence the LODs (tr_wall_lod, meshes.h). */
static void walls(tr_dl_t *dl, const tr_cam_t *cam, const tr_light_t *l, uint32_t idx, float z0, uint8_t q,
		  const tr_frame_in_t *in)
{
	int              lo   = z0 > ((q & TR_LOD_NEAR) ? TR_LOD_NEAR_Z : LOD_Z);
	const tr_mesh_t *cap  = lo ? &tr_mesh_cap_lo : &tr_mesh_cap;
	const tr_mesh_t *dark = lo ? &tr_mesh_cap_dark_lo : &tr_mesh_cap_dark;

	for (int side = -1; side <= 1; side += 2) {
		tr_deco_t d;

		tr_scene_deco(idx, side, z0, q, in, &d); /* the tile hash, and the deco it picks */
		uint32_t h   = d.h;
		float    zc  = z0 + (float)TR_TILE_LEN * 0.5f, sx = (float)side;
		float    yaw = (float)((h >> 8) & 7u) * (PI_F / 4.0f);
		float    tw  = (float)((int32_t)((h >> 8) & 7u) - 4) * 0.06f; /* square parts: a slight twist */

		switch (h & 7u) {
		case 0:
		case 6:
			wall(dl, cam, l, cap, sx * 480.0f, zc, yaw, 1.0f);
			break;
		case 1:
			wall(dl, cam, l, dark, sx * 480.0f, zc - 80.0f, yaw, 0.8f);
			wall(dl, cam, l, &tr_mesh_cap_dark_lo, sx * 470.0f, zc + 90.0f, yaw + 1.0f, 0.6f);
			break;
		case 3:
			wall(dl, cam, l, lo ? &tr_mesh_qfp_lo : &tr_mesh_qfp, sx * 510.0f, zc, tw, 1.0f);
			break;
		case 4:
			wall(dl, cam, l, lo ? &tr_mesh_bga_lo : &tr_mesh_bga, sx * 500.0f, zc, tw, 1.0f);
			break;
		case 5:
			wall(dl, cam, l, lo ? &tr_mesh_inductor_lo : &tr_mesh_inductor, sx * 490.0f, zc - 60.0f, tw, 1.0f);
			wall(dl, cam, l, lo ? &tr_mesh_crystal_lo : &tr_mesh_crystal, sx * 470.0f, zc + 110.0f,
			     PI_F * 0.5f + tw, 1.0f);
			break;
		default:
			wall(dl, cam, l, lo ? &tr_mesh_dip_lo : &tr_mesh_dip, sx * 500.0f, zc, 0, 1.0f);
			break;
		}
		if (z0 < SMALL_Z && ((h >> 16) & 1u)) { /* an 0603 + SOT-23, or a jumper wire, at the lane edge */
			if ((h >> 17) & 1u) {
				wall(dl, cam, l, &tr_mesh_smd, sx * 385.0f, zc + 130.0f, sx * PI_F * 0.5f, 1.0f);
			} else {
				wall(dl, cam, l, &tr_mesh_jumper, sx * 385.0f, zc - 110.0f, PI_F * 0.5f, 1.0f);
			}
		}
		if (((h >> 12) & 1u) && !(q & TR_LOD_NO_BACK_RANK)) { /* back rank: half hidden by the front one, always LOD */
			wall(dl, cam, l, (h >> 13) & 1u ? &tr_mesh_cap_lo : &tr_mesh_cap_dark_lo, sx * 760.0f, zc, yaw,
			     TR_WALL_SY_MAX - (float)(3u - ((h >> 14) & 3u)) * 0.25f);
		}
		if (!(q & TR_LOD_STILL)) {
			deco(dl, cam, l, &d, side);
		}
	}
}

/* ------------------------------------------------------ zone scenery (P15)
 * The other zones' shoulders, by the same per-tile hash as walls(): parts
 * from zones.h (left or right-hand copy, far LOD past the LOD depth, none
 * where a part has no LOD), culled like wall(). */

/* An unlit copy of l: emissive colours (neon, glow, rings) at full
 * strength, fogged to FAR_FOG_END on the plain curve (no far knee: a glow
 * never reads brighter through the haze than the world behind it did). */
static tr_light_t unlit(const tr_light_t *l)
{
	tr_light_t u = *l;

	u.ambient = 1.0f;
	u.fog_end = FAR_FOG_END;
	u.fog_far = 0.0f;
	return u;
}

static void zpart(tr_dl_t *dl, const tr_cam_t *cam, const tr_light_t *l, int id, int side, int lo, float x, float z,
		  float sy)
{
	const tr_zpart_t *p = &tr_zpart[id];
	const tr_mesh_t  *m = p->m[side > 0][lo];
	float             k = tr_scene_wall_r * (1.0f / TR_WALL_R) * (sy > 1.0f ? sy : 1.0f); /* the test hook scales it */

	if (m != NULL && id != TR_ZPART_MAST && as_bb(z)) { /* a mast is a lattice: its 10-tri low mesh reads, a quad does not */
		far_bb(dl, cam, l, m, x, z, sy);
	} else if (m != NULL && !off_screen(cam, (tr_v3_t){x, p->cy * sy, z}, p->r * k)) {
		emit(dl, cam, l, m, x, 0, z, 0, sy);
	}
}

/* Wave rings round an antenna mast at (x, z): RING_N rings expanding from
 * RING_R0 to RING_R1 every 24 ticks (RING_TQ), flat on the board, the zone's
 * ACCENT fading to the board colour as they spread; the half toward the
 * lanes only (the other half is off screen or behind the mast). */
#define RING_N     3
#define RING_SEG   8
#define RING_R0    60.0f
#define RING_R1    720.0f
#define RING_W     9.0f
#define RING_TQ    384u /* 24 ticks in 1/16 ticks (time_q4()) */
static void rings(tr_dl_t *dl, const tr_cam_t *cam, const tr_light_t *lu, int side, float x, float z,
		  const tr_frame_in_t *in)
{
	const uint16_t *pal = pal_of(lu);
	float           tf  = (float)(time_q4(in) % RING_TQ) / (float)RING_TQ; /* integer: never wraps mid-life */

	for (int k = 0; k < RING_N; k++) {
		float f = tf + (float)k / (float)RING_N;

		f -= (float)(int32_t)f; /* 0 .. 1 over a ring's life */
		float    r  = RING_R0 + (RING_R1 - RING_R0) * f;
		uint16_t c  = mix565(pal[ACCENT_IDX], pal[2], q8(f * f));
		float    vz = view_z(cam, x, z);

		c = glow_fog(lu, c, vz, 0.7f);
		for (int i = 0; i < RING_SEG; i++) {
			/* angles from +z toward the lanes (x = -side), half a turn */
			float a0 = PI_F * (float)i / (float)RING_SEG, a1 = PI_F * (float)(i + 1) / (float)RING_SEG;
			float s0, c0, s1, c1, ri = r - RING_W, ro = r + RING_W, d = -(float)side;

			tr_sincosf(a0, &s0, &c0);
			tr_sincosf(a1, &s1, &c1);
			/* inner -> outer at a0, outer -> inner at a1: up-facing for
			 * a1 turning clockwise seen from above (ground_run()'s order) */
			tr_v3_t q[4] = {{x + d * ri * s0, 0.6f, z + ri * c0}, {x + d * ro * s0, 0.6f, z + ro * c0},
					{x + d * ro * s1, 0.6f, z + ro * c1}, {x + d * ri * s1, 0.6f, z + ri * c1}};

			if (d < 0.0f) { /* mirrored: keep the winding */
				tr_v3_t t = q[1];

				q[1] = q[3], q[3] = t;
			}
			tr_r3d_emit_quad(dl, cam, q, c, 0);
		}
	}
}

static void zone_walls(tr_dl_t *dl, const tr_cam_t *cam, const tr_light_t *l, uint8_t zone, uint32_t idx, float z0,
		       uint8_t q, const tr_frame_in_t *in)
{
	tr_light_t lu = unlit(l);
	int               lo = z0 > ((q & TR_LOD_NEAR) ? TR_LOD_NEAR_Z : LOD_Z);
	int               back = !(q & TR_LOD_NO_BACK_RANK);

	for (int side = -1; side <= 1; side += 2) {
		uint32_t h  = hash(idx * 2u + (side > 0));
		float    zc_ = z0 + (float)TR_TILE_LEN * 0.5f, sx = (float)side;
		float    vy = 0.85f + 0.1f * (float)((h >> 5) & 3u); /* height variety */

		switch (zone) {
		case TR_ZONE_DIE: {
			/* Die towers with their bond wires down to the lane edge,
			 * an open plaza now and then; a back rank of taller stacks. */
			static const float half[3] = {100.0f, 110.0f, 120.0f}; /* genzone.py die_tower() w / 2 */
			uint32_t           k       = (h >> 3) % 3u;

			if ((h & 3u) != 0u) {
				zpart(dl, cam, l, TR_ZPART_DIE0 + (int)k, side, lo, sx * (500.0f + half[k]), zc_, vy);
			}
			if (back && ((h >> 12) & 3u) == 0u) {
				zpart(dl, cam, l, TR_ZPART_DIE2, side, 1, sx * 980.0f, zc_, 1.0f + 0.2f * (float)((h >> 13) & 3u));
			}
			break;
		}
		case TR_ZONE_MEM:
			/* A row of DIMMs per side, end to end down the canyon, a
			 * taller rank showing over it here and there. */
			zpart(dl, cam, l, TR_ZPART_DIMM0 + (int)(h & 1u), side, lo, sx * 430.0f, zc_, 1.0f);
			if (back && ((h >> 12) & 3u) == 0u) {
				zpart(dl, cam, l, TR_ZPART_DIMM1, side, 1, sx * 560.0f, zc_, 1.3f);
			}
			break;
		case TR_ZONE_RF:
			/* Open field: a mast every few tiles with its rings, a dish
			 * now and then, whips at the lane edge near by. */
			if (h % 4u == 0u) {
				zpart(dl, cam, l, TR_ZPART_MAST, side, lo, sx * 780.0f, zc_, vy);
				if (z0 < LED_Z && !(q & TR_LOD_STILL)) {
					rings(dl, cam, &lu, side, sx * 780.0f, zc_, in);
				}
			} else if (h % 4u == 1u) {
				zpart(dl, cam, l, TR_ZPART_DISH, side, lo, sx * 600.0f, zc_, 1.0f);
			}
			if (z0 < SMALL_Z && ((h >> 16) & 1u)) {
				zpart(dl, cam, l, TR_ZPART_WHIP, side, 0, sx * 400.0f, zc_ + 100.0f, 1.0f);
			}
			if (back && ((h >> 12) & 7u) == 0u) {
				zpart(dl, cam, l, TR_ZPART_MAST, side, 1, sx * 1400.0f, zc_, 1.3f);
			}
			break;
		default: { /* TR_ZONE_NEON */
			/* Dark towers along the street, their lane-side edges in
			 * neon (unlit), a back rank of taller dark ones. */
			static const float half[3] = {100.0f, 115.0f, 130.0f}; /* genzone.py neon_tower() w / 2 */
			uint32_t           k       = (h >> 3) % 3u;
			float              x       = sx * (440.0f + half[k]);

			if ((h & 7u) == 0u) {
				break; /* a cross street */
			}
			zpart(dl, cam, l, TR_ZPART_TOWER0 + 2 * (int)k, side, lo, x, zc_, vy);
			if (!lo) {
				zpart(dl, cam, &lu, TR_ZPART_STRIP0 + 2 * (int)k, side, 0, x, zc_, vy);
			}
			if (back && ((h >> 12) & 3u) == 0u) {
				zpart(dl, cam, l, TR_ZPART_TOWER1, side, 1, sx * 1000.0f, zc_, 1.2f + 0.2f * (float)((h >> 13) & 3u));
			}
			break;
		}
		}
	}
}

/* The gate to the incoming zone at the frame's gate depth: its frame lit
 * in that zone's palette, the glow inside it and a threshold bar across the
 * ground (over the ground seam, ground_near(): >= NEAR_GRID / 2 either side)
 * in its ACCENT, half the board's colour, melting into the board as it
 * nears the camera (a full-width emissive band under the lens is a flash,
 * not a threshold). Hazed like the world at its depth, and faded in from
 * the road's colour over its first ENT_FADE_STEPS steps from its spawn row
 * (zone.h TR_ZONE_GATE_Y), as a part is: it rises out of the far road
 * (scale_y 0 -> 1 over the same steps). */
#define GATE_BAR 20.0f
_Static_assert(GATE_BAR >= NEAR_GRID / 2, "the bar must cover the ground seam's snap");
static void gate(tr_dl_t *dl, const tr_cam_t *cam, const zctx_t *zc, const tr_frame_in_t *in_, float eye_z)
{
	float      gz = zc->gate_z;
	tr_light_t lf = zc->l[1], lu;

	if (zc->near == zc->far || gz < eye_z) {
		return;
	}
	uint16_t        gp[16];
	const uint16_t *p    = pal_of(&zc->lg[1]);
	float           vz   = view_z(cam, 0.0f, gz);
	float           uf   = clampf((gz - (float)tr_proj_depth_of_model_y(TR_ZONE_GATE_Y)) / (ENT_FADE_STEPS * dz_tick()) + 1.0f,
				      0.0f, 1.0f);
	float           f    = uf * (2.0f - uf);
	uint16_t        road = glow_fog(&zc->lg[1], p[BOARD_IDX], vz, 1.0f);

	lf = fade_light(&zc->l[1], &zc->lg[1], vz, f);
	lu         = lf;
	lu.ambient = 1.0f; /* the glow: emissive */
	emit(dl, cam, &lf, &tr_zmesh_gate, 0.0f, 0.0f, gz, 0.0f, 1.0f - uf); /* rising out of the road */
	/* the glow breathes toward white, ~2 Hz at the play pace */
	memcpy(gp, pal_of(&lu), sizeof(gp));
	gp[ACCENT_IDX] = mix565(gp[ACCENT_IDX], RGB565(255, 255, 255),
				(uint32_t)(60.0f + 60.0f * sinf_((float)((time_q4(in_) * 391u) & 0xFFFFu) * (2.0f * PI_F / 65536.0f))));
	lu.pal = gp;
	emit(dl, cam, &lu, &tr_zmesh_gate_glow, 0.0f, 0.0f, gz, 0.0f, 1.0f - uf);
	{
		float   x = 10.5f * (float)TR_PROJ_LANE_W;
		tr_v3_t q[4] = {{-x, 0, gz - GATE_BAR}, {-x, 0, gz + GATE_BAR}, {x, 0, gz + GATE_BAR}, {x, 0, gz - GATE_BAR}};

		const uint16_t *pu  = pal_of(&zc->l[1]);
		uint16_t        bar = mix565(pu[ACCENT_IDX], pu[2], 96u + q8(clampf((700.0f - vz) / 500.0f, 0.0f, 1.0f)) * 160u / 256u);

		bar = glow_fog(&zc->lg[1], bar, vz, 1.0f);
		tr_r3d_emit_quad(dl, cam, q, f > 0.0f ? mix565(bar, road, q8(f)) : bar, TR_TRI_NOZ);
	}
}

/* Fades DL triangles [n0, dl->n) toward rgb by a / 256: a far-layer piece
 * of the outgoing (or incoming) zone melting into the haze. */
static void fade_tris(tr_dl_t *dl, uint16_t n0, uint16_t rgb, uint32_t a)
{
	for (uint16_t i = n0; i < dl->n; i++) {
		tr_tri_t *t = &dl->tri[i];

		t->c = mix565(t->c, rgb, a);
		for (int k = 0; k < 3; k++) {
			t->a[k].rgb = mix565(t->a[k].rgb, rgb, a);
		}
	}
}

/* ------------------------------------------------------------ live wires (P4b)
 * A live wire hangs between two posts at the lane edges: high, strung at
 * chest height and sagging (duck it), or low, lying across the track and
 * writhing (jump it). An electric arc runs along it -- a jagged polyline of
 * thin quads, a white core over a blue glow -- re-randomised every frame
 * from tr_scene_wire_seed(); sparks spray where it touches the board or its
 * posts. Arcs and sparks are emissive: flat colours through
 * tr_r3d_emit_quad() (unlit), only half fogged, so they read far off. */
#define WIRE_HALF  104.0f /* posts at the lane centre +- this */
#define WIRE_SAG   28.0f
#define ARC_CORE   RGB565(236, 252, 255)
#define ARC_GLOW   RGB565(60, 140, 255)
#define ARC_THIN   RGB565(150, 220, 255)
#define SPARK_HOT  RGB565(255, 246, 200)
#define SPARK_WARM RGB565(255, 176, 72)
#define WIRE_CU    RGB565(214, 132, 60)
#define WIRE_SEGS  8

uint32_t tr_scene_wire_seed(const tr_frame_in_t *in, int i)
{
	return hash(in->tick * 0x9E3779B1u ^ (uint32_t)phase_q16(in) * 0x85EBCA77u ^ (uint32_t)in->crash_tick * 0x27D4EB2Fu ^
		    (uint32_t)i * 0xC2B2AE3Du);
}

void tr_scene_arc(uint32_t seed, tr_v3_t a, tr_v3_t b, float bow, float amp, int n, tr_v3_t *out)
{
	uint32_t r = hash(seed) ^ 0x2545F491u;

	r = r ? r : 1u; /* xorshift's one fixed point */

	for (int k = 0; k <= n; k++) {
		float t = (float)k / (float)n, e = 4.0f * t * (1.0f - t); /* 0 at the ends, 1 mid-way */

		out[k] = (tr_v3_t){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t - bow * e, a.z + (b.z - a.z) * t};
		if (k > 0 && k < n) {
			uint32_t h = xorshift(&r);
			float    j = amp * (e > 0.35f ? 1.0f : e / 0.35f); /* jitter pinned toward the ends */

			out[k].x += j * 0.4f * ((float)(h & 255u) / 127.5f - 1.0f);
			out[k].y += j * ((float)((h >> 8) & 255u) / 127.5f - 1.0f);
			out[k].z += j * 0.3f * ((float)((h >> 16) & 255u) / 127.5f - 1.0f);
		}
	}
}

/* Entity mesh m at (x, y, z) through the far scale ef->k about ef->base
 * (uniform: the mesh's normals stay valid). */
#define ENT_MAX_V 256 /* > tr_mesh_resistor's 164, the largest entity mesh */
static float ent_v[ENT_MAX_V * 3];

static void ent_emit(tr_dl_t *dl, const tr_cam_t *cam, const ent_fog_t *ef, const tr_mesh_t *m, float x, float y,
		     float z, float yaw)
{
	const tr_v3_t *b  = &ef->base;
	tr_mesh_t      sm = *m;

	if (ef->k == 1.0f || m->nv > ENT_MAX_V) {
		emit(dl, cam, &ef->l, m, x, y, z, yaw, 1.0f);
		return;
	}
	for (int i = 0; i < m->nv * 3; i++) {
		ent_v[i] = (m->vf ? m->vf[i] : (float)m->v[i]) * ef->k;
	}
	sm.vf = ent_v;
	emit(dl, cam, &ef->l, &sm, b->x + (x - b->x) * ef->k, b->y + (y - b->y) * ef->k, b->z + (z - b->z) * ef->k, yaw,
	     1.0f);
}

/* Far entity LOD (past ENT_FAR_Z, a part ~5 s and more out, a few dozen
 * px): a camera-facing quad from (x0, y0) to (x1, y1) about the part's
 * centre line at depth z, far-scaled about its base like ent_emit(), in
 * palette colour col at a lit side's average, fogged as the part. The arch
 * is its two posts and lintel (6 triangles, not 48), the resistor its body
 * and legs (6, not 32), a wire's post its stem and head (4, not 18) and its
 * wire and arcs in half the segments: every far part keeps its own shape
 * (tr_far_quad_t, r3d_scene.h). */
#define ENT_FAR_Z 9000.0f
const tr_far_quad_t tr_far_arch[3]      = {{-112.0f, 0.0f, -94.0f, 150.0f, 15}, {94.0f, 0.0f, 112.0f, 150.0f, 15},
					   {-112.0f, 150.0f, 112.0f, 172.0f, 15}};
const tr_far_quad_t tr_far_resistor[3]  = {{-84.0f, 26.0f, 84.0f, 82.0f, 13}, {-112.0f, 0.0f, -100.0f, 58.0f, 6},
					   {100.0f, 0.0f, 112.0f, 58.0f, 6}};
const tr_far_quad_t tr_far_post_low[2]  = {{-5.0f, 0.0f, 5.0f, 28.0f, 6}, {-11.0f, 28.0f, 11.0f, 48.0f, 7}};
const tr_far_quad_t tr_far_post_high[2] = {{-5.0f, 0.0f, 5.0f, 180.0f, 6}, {-11.0f, 180.0f, 11.0f, 200.0f, 7}};

static void ent_quads(tr_dl_t *dl, const tr_cam_t *cam, const ent_fog_t *ef, float x, float z, const tr_far_quad_t *q, int n);
static void ent_quad(tr_dl_t *dl, const tr_cam_t *cam, const ent_fog_t *ef, float x, float x0, float y0, float x1, float y1,
		     float z, uint8_t col)
{
	const tr_v3_t  *b   = &ef->base;
	const uint16_t *pal = ef->l.pal ? ef->l.pal : tr_r3d_palette;
	float           in  = ef->l.ambient + (1.0f - ef->l.ambient) * 0.6f, k = ef->k;
	uint16_t        c   = pal[col & 15u];
	uint16_t        cl  = (uint16_t)((uint32_t)((float)(c >> 11) * in) << 11 | (uint32_t)((float)((c >> 5) & 63u) * in) << 5 |
					 (uint32_t)((float)(c & 31u) * in));
	float           X0 = b->x + (x + x0 - b->x) * k, X1 = b->x + (x + x1 - b->x) * k;
	float           Y0 = b->y + (y0 - b->y) * k, Y1 = b->y + (y1 - b->y) * k;
	tr_v3_t         q[4] = {{X1, Y0, z}, {X0, Y0, z}, {X0, Y1, z}, {X1, Y1, z}};

	tr_r3d_emit_quad(dl, cam, q, glow_fog(&ef->l, cl, view_z(cam, x, z), 1.0f), 0);
}

static void ent_quads(tr_dl_t *dl, const tr_cam_t *cam, const ent_fog_t *ef, float x, float z, const tr_far_quad_t *q, int n)
{
	for (int i = 0; i < n; i++) {
		ent_quad(dl, cam, ef, x, q[i].x0, q[i].y0, q[i].x1, q[i].y1, z, q[i].col);
	}
}

/* One thin screen-facing quad from p to q, half width hw (in the xy plane,
 * so it faces the camera at any heading), colour c fogged by fk x the
 * entity's haze and by its fade-in. Far LOD: past SEG_KEEP_Z (camera
 * depth) an emissive stroke (fk < 1: arcs, sparks) keeps the screen width
 * it had there instead of thinning to a sub-pixel line, so a far wire's arc
 * still reads; the copper wire itself thins as it should. */
/* SEG_KEEP_Z is a camera depth (view_z), where LOD_Z and ent_fog()'s
 * depths are world z: the eye stands at world z -104, so on the road this is
 * ~world 1,900, a little past LOD_Z (1,800) -- a line-width floor, not a LOD
 * switch. */
#define SEG_KEEP_Z 2000.0f
static void seg(tr_dl_t *dl, const tr_cam_t *cam, const ent_fog_t *ef, tr_v3_t p, tr_v3_t q, float hw, uint16_t c,
		float fk)
{
	float dx = q.x - p.x, dy = q.y - p.y, d2 = dx * dx + dy * dy;

	if (d2 < 1e-4f) {
		return;
	}
	float vz = view_z(cam, (p.x + q.x) * 0.5f, (p.z + q.z) * 0.5f) + cam->view.m[2][1] * (p.y + q.y) * 0.5f;

	if (fk < 1.0f && vz > SEG_KEEP_Z) { /* emissive only; grown in, as the far scale */
		hw *= 1.0f + (vz / SEG_KEEP_Z - 1.0f) * ef->grown;
	}
	if (ef->k != 1.0f) { /* the far scale, as ent_emit() */
		tr_v3_t b = ef->base;

		p  = (tr_v3_t){b.x + (p.x - b.x) * ef->k, b.y + (p.y - b.y) * ef->k, b.z + (p.z - b.z) * ef->k};
		q  = (tr_v3_t){b.x + (q.x - b.x) * ef->k, b.y + (q.y - b.y) * ef->k, b.z + (q.z - b.z) * ef->k};
		hw *= ef->k;
	}
	/* n: the direction turned +90 degrees, |n| = hw -- 1/sqrt from the
	 * bit-trick guess and three Newton steps (IEEE ops only: host == A32
	 * bits, no libm); p - n, p + n, q + n, q - n is front facing for any
	 * direction. */
	uint32_t bits;
	float    inv;

	memcpy(&bits, &d2, 4);
	bits = 0x5F3759DFu - (bits >> 1);
	memcpy(&inv, &bits, 4);
	for (int k = 0; k < 3; k++) {
		inv = inv * (1.5f - 0.5f * d2 * inv * inv);
	}
	float   nx = -dy * inv * hw, ny = dx * inv * hw;
	tr_v3_t  qd[4] = {{p.x - nx, p.y - ny, p.z}, {p.x + nx, p.y + ny, p.z}, {q.x + nx, q.y + ny, q.z},
			 {q.x - nx, q.y - ny, q.z}};

	for (int k = 0; k < 4; k++) {
		qd[k].y = qd[k].y > 0.0f ? qd[k].y : 0.0f; /* cut at the board: the NOZ ground hides nothing */
	}
	/* an entity's fog: its haze (x fk) toward the glow, its fade-in toward the road */
	uint32_t h = (uint32_t)(256.0f * fk * ef->haze), f = (uint32_t)(256.0f * ef->fade);

	c = h ? mix565(c, ef->glow, h > 256u ? 256u : h) : c;
	tr_r3d_emit_quad(dl, cam, qd, f ? mix565(c, ef->road, f > 256u ? 256u : f) : c, 0);
}

/* An arc through pts[0..n]: the core, then the glow behind it (a coplanar
 * tie keeps the first drawn). */
static void arc(tr_dl_t *dl, const tr_cam_t *cam, const ent_fog_t *ef, const tr_v3_t *pts, int n, float w)
{
	for (int k = 0; k < n; k++) {
		seg(dl, cam, ef, pts[k], pts[k + 1], 2.4f * w, ARC_CORE, 0.4f);
	}
	for (int k = 0; k < n; k++) {
		seg(dl, cam, ef, pts[k], pts[k + 1], 7.0f * w, ARC_GLOW, 0.6f);
	}
}

/* Stateless spark j of a burst from `src`: ballistic, one life every
 * `period` frames of scene time tf, direction from the hash. */
static void spark(tr_dl_t *dl, const tr_cam_t *cam, const ent_fog_t *ef, tr_v3_t src, float tf, uint32_t j, float up)
{
	uint32_t h   = hash(j * 0x9E37u + 17u);
	float    per = 9.0f + (float)(h & 7u);
	float    age = tf + (float)((h >> 3) & 63u);

	age -= per * (float)(int32_t)(age / per); /* 0 .. per */
	uint32_t b  = hash(h ^ (uint32_t)(int32_t)((tf + (float)((h >> 3) & 63u)) / per)); /* this life's direction */
	float    vx = ((float)(b & 255u) / 127.5f - 1.0f) * 6.0f, vz = -((float)((b >> 8) & 127u) / 127.0f) * 3.0f;
	float    vy = up * (2.0f + (float)((b >> 16) & 255u) / 64.0f);
	tr_v3_t  p  = {src.x + vx * age, src.y + vy * age - 0.35f * age * age, src.z + vz * age};

	if (p.y < 1.5f) {
		return; /* landed (the NOZ board hides nothing below it) */
	}
	float   r = 3.6f - 0.15f * age;
	tr_v3_t q = {p.x - vx * 1.6f, p.y - (vy - 0.7f * age) * 1.6f, p.z}; /* a streak */

	seg(dl, cam, ef, q, p, r > 1.6f ? r : 1.6f, age < per * 0.4f ? SPARK_HOT : SPARK_WARM, 0.3f);
}

static void live_wire(tr_dl_t *dl, const tr_cam_t *cam, const ent_fog_t *ef, const tr_scene_t *s,
		      const tr_frame_in_t *in, int i, float x, float z, int lo, int hit)
{
	int              low  = in->ents[i].low != 0;
	const tr_mesh_t *post = low ? (lo ? &tr_mesh_post_low_lo : &tr_mesh_post_low)
				    : (lo ? &tr_mesh_post_high_lo : &tr_mesh_post_high);
	float            y0   = low ? TR_WIRE_Y_LOW : TR_WIRE_Y_HIGH;
	uint32_t         seed = tr_scene_wire_seed(in, i);
	float            tf   = (float)in->tick + phase_f(in) + (in->flags & TR_FLAG_CRASH ? crash_time(in) : 0.0f);
	int              n    = z > ENT_FAR_Z ? WIRE_SEGS / 4 : lo ? WIRE_SEGS / 2 : WIRE_SEGS;
	tr_v3_t          w[WIRE_SEGS + 1], p[WIRE_SEGS + 1], q[WIRE_SEGS + 1]; /* wire, arc, strand/fork */
	tr_v3_t          ta = {x - WIRE_HALF, y0, z}, tb = {x + WIRE_HALF, y0, z};

	if (z > ENT_FAR_Z) { /* the posts as quads: stem and head */
		ent_quads(dl, cam, ef, ta.x, z, low ? tr_far_post_low : tr_far_post_high, 2);
		ent_quads(dl, cam, ef, tb.x, z, low ? tr_far_post_low : tr_far_post_high, 2);
	} else {
		ent_emit(dl, cam, ef, post, ta.x, 0, z, 0);
		ent_emit(dl, cam, ef, post, tb.x, 0, z, 0);
	}
	/* The wire, terminal to terminal: high, a sagging span swaying a
	 * little; low, dropping off both terminals onto the board and
	 * writhing along it (lifting off and slapping down). */
	for (int k = 0; k <= n; k++) {
		float t = (float)k / (float)n, y;

		if (low) {
			float ea = 1.0f - 4.0f * t, eb = 4.0f * t - 3.0f;
			float drop = (ea > 0.0f ? ea * ea : 0.0f) + (eb > 0.0f ? eb * eb : 0.0f);
			float env  = 1.0f - drop;
			float lift = sinf_(tf * 1.3f + t * 7.0f + (float)i);

			y    = 5.0f + (y0 - 5.0f) * drop + (lift > 0.0f ? 9.0f * lift * env : 0.0f);
			w[k] = (tr_v3_t){ta.x + 2.0f * WIRE_HALF * t, y, z + 16.0f * env * sinf_(tf * 0.9f + t * 9.0f + (float)i)};
		} else {
			y    = y0 - WIRE_SAG * 4.0f * t * (1.0f - t) + 3.0f * sinf_(tf * 0.7f + t * 3.0f) * 4.0f * t * (1.0f - t);
			w[k] = (tr_v3_t){ta.x + 2.0f * WIRE_HALF * t, y, z};
		}
	}
	for (int k = 0; k < n; k++) {
		seg(dl, cam, ef, w[k], w[k + 1], low ? 4.5f : 3.2f, WIRE_CU, 1.0f);
	}
	if (!low) {
		/* The arc runs the span just in front of the wire, bowed like it,
		 * and a branch forks off it on odd seeds. */
		tr_v3_t a = {ta.x, y0 + 4.0f, z - 8.0f}, b = {tb.x, y0 + 4.0f, z - 8.0f};

		tr_scene_arc(seed, a, b, WIRE_SAG - 6.0f, 16.0f, n, p);
		arc(dl, cam, ef, p, n, 1.0f);
		if (!lo) { /* a second, thinner strand */
			tr_scene_arc(seed ^ 0x5A5A5A5Au, a, b, WIRE_SAG - 2.0f, 22.0f, n, q);
			for (int k = 0; k < n; k++) {
				seg(dl, cam, ef, q[k], q[k + 1], 1.5f, ARC_THIN, 0.4f);
			}
		}
		if (seed & 0x100u && !lo) {
			int     k0 = 2 + (int)((seed >> 9) % (uint32_t)(n - 4));
			tr_v3_t e  = {p[k0].x + (float)((seed >> 12) & 63u) - 31.5f, p[k0].y + 30.0f, p[k0].z};

			tr_scene_arc(seed >> 3, p[k0], e, 0.0f, 9.0f, 3, q);
			arc(dl, cam, ef, q, 3, 0.7f);
		}
	} else {
		/* An arc crawling along the wire, just above and in front of it,
		 * and two from the wire down to the board where it lies, at points
		 * that move every frame. */
		uint32_t r = seed | 1u;

		for (int k = 0; k <= n; k++) {
			uint32_t h = xorshift(&r);

			p[k] = (tr_v3_t){w[k].x + (float)(h & 15u) - 7.5f, w[k].y + 3.0f + (float)((h >> 4) & 15u) * 0.7f,
					 w[k].z - 5.0f};
		}
		p[0] = w[0], p[n] = w[n];
		arc(dl, cam, ef, p, n, 0.8f);
		for (int k = 0; k < 2; k++) {
			int     at = 1 + (int)((seed >> (4 * k)) % (uint32_t)(n - 1));
			tr_v3_t g  = {w[at].x + (float)((seed >> (8 + 5 * k)) & 31u) - 15.5f, 0.5f, w[at].z - 4.0f};
			tr_v3_t tp = {w[at].x, w[at].y + 2.0f, w[at].z - 4.0f};

			tr_scene_arc(seed >> (k + 1), tp, g, 0.0f, 7.0f, 3, p);
			arc(dl, cam, ef, p, 3, 1.0f);
			if (!lo) {
				for (uint32_t j = 0; j < 4; j++) {
					spark(dl, cam, ef, g, tf, (uint32_t)i * 8u + (uint32_t)k * 3u + j, 0.8f);
				}
			}
		}
	}
	if (!lo && !low) { /* sparks off both terminals */
		for (uint32_t j = 0; j < 6; j++) {
			spark(dl, cam, ef, j & 1u ? tb : ta, tf, (uint32_t)i * 8u + j, 0.4f);
		}
	}
	if (hit && crash_time(in) < 30.0f) {
		/* Struck: arcs from the wire (w[], untouched above) into the
		 * runner's chest. */
		for (int k = 0; k < 2; k++) {
			tr_v3_t a = w[k ? n / 4 : 3 * n / 4];
			tr_v3_t b = {s->runner_x + (k ? -22.0f : 22.0f), low ? 30.0f : 120.0f, RUNNER_Z + TR_RUNNER_FRONT_Z - 10.0f};

			a.z -= 6.0f;
			tr_scene_arc(seed >> (k * 5), a, b, -10.0f, 12.0f, 6, p);
			arc(dl, cam, ef, p, 6, 1.2f);
		}
	}
}

float tr_scene_ent_z(const tr_frame_in_t *in, int i)
{
	/* One tick is dz_tick() of depth; the sub-tick phase moves it on. */
	float z = (float)tr_proj_depth_of_model_y(in->ents[i].y) - phase_f(in) * dz_tick();

	const tr_pkt_ent_t *e = &in->ents[i];
	/* An obstacle still coming at the runner in its lane that the current
	 * pose does not clear will hit it: hold it at the runner's front from
	 * the frames BEFORE the fatal tick (whose phase extrapolation would
	 * otherwise push it inside the runner), so the fatal frame does not
	 * jump it backwards. Past the runner line (step.c's band) it was
	 * cleared, and is left alone. */
	int ahead = (in->flags & TR_FLAG_ALIVE) && (e->kind == 1 || e->kind == 3) && e->lane == in->lane &&
		    e->y < tr_runner_ground_y(TR_R3D_H) &&
		    !(in->flags & (e->low ? TR_FLAG_AIRBORNE : TR_FLAG_DUCKING));

	if ((ahead || ((in->flags & TR_FLAG_CRASH) && i == in->crash_ent)) && z < RUNNER_Z + CONTACT_DZ) {
		z = RUNNER_Z + CONTACT_DZ; /* stopped against the runner's front, not inside it */
	}
	return z;
}

float tr_scene_particle_r(const tr_particle_t *p)
{
	if (p->life <= 0.0f || (p->spark && p->pos.z < RUNNER_Z - 40.0f)) {
		return 0.0f;
	}
	/* Shrink to fade. Sparks stay small (r <= ~8) and are culled once
	 * nearer than the runner: a big quad at the lens is a costly
	 * full-screen fill, and reads as a white square, not a spark. */
	return p->spark ? 2.0f + 0.2f * p->life : 3.0f + 0.6f * p->life;
}

static uint16_t flash_rgb(const tr_frame_in_t *in)
{
	return in->crash_kind == TR_CRASH_KIND_WIRE ? FLASH_WIRE : FLASH_RGB;
}

/* Flash strength at crash time `ct`: 1 at the hit, quadratic fade to 0. */
static float flash_amount(float ct)
{
	float f = 1.0f - clampf(ct / FLASH_FRAMES, 0.0f, 1.0f);

	return f * f;
}

/* One flat screen-space rectangle nearer than everything (w 65535), two
 * clockwise tris straight into the DL -- no camera, no clipping. */
static void screen_rect(tr_dl_t *dl, int x0, int y0, int x1, int y1, uint16_t c)
{
	tr_sv_t  a = {x0 << TR_R3D_SUB, y0 << TR_R3D_SUB}, b = {x1 << TR_R3D_SUB, y0 << TR_R3D_SUB};
	tr_sv_t  d = {x1 << TR_R3D_SUB, y1 << TR_R3D_SUB}, e = {x0 << TR_R3D_SUB, y1 << TR_R3D_SUB};
	tr_tri_t t[2] = {{{a, b, d}, c, 0, 0, {{0xFFFF, 0, 0, c}, {0xFFFF, 0, 0, c}, {0xFFFF, 0, 0, c}}},
			 {{a, d, e}, c, 0, 0, {{0xFFFF, 0, 0, c}, {0xFFFF, 0, 0, c}, {0xFFFF, 0, 0, c}}}};

	for (int k = 0; k < 2; k++) {
		if (dl->n < TR_DL_MAX_TRIS) {
			dl->tri[dl->n++] = t[k];
		} else {
			tr_dl_dropped++;
		}
	}
}

/* The crash's flash (red, blue-white for a live wire): a screen border
 * that thins out as it fades (a flat fill costs only its own pixels; no
 * blend pass). */
static void flash_border(tr_dl_t *dl, float ct, uint16_t c)
{
	int th = (int)((float)FLASH_PX * flash_amount(ct));

	if (th <= 0) {
		return;
	}
	screen_rect(dl, 0, 0, TR_R3D_W, th, c);
	screen_rect(dl, 0, TR_VIEW_H - th, TR_R3D_W, TR_VIEW_H, c);
	screen_rect(dl, 0, th, th, TR_VIEW_H - th, c);
	screen_rect(dl, TR_R3D_W - th, th, TR_R3D_W, TR_VIEW_H - th, c);
}

/* World units hold_front() pulled the drawn runner's pos.z back by this
 * frame (0 if it didn't clamp): test instrumentation only (tr_scene_runner()
 * resets it), see r3d_scene.h. Not roll_about_middle()'s own clamp just
 * above -- a rolled/curled pose legitimately needs that one; this counts
 * only the work hold_front() still finds after it, which should be near
 * nothing once roll_about_middle() measures the front right. */
float tr_front_pullback;

/* Turns the skinned runner `a` radians forward (pitch) about the middle of
 * its bounding box: the vertices are centred on it and the instance moved
 * there (along its yaw, so angle 0 is the same picture). The front is
 * then held inside TR_RUNNER_FRONT_Z -- a rolled ball reaches further
 * forward than it stands, and an obstacle held at the contact depth must
 * not go into it. */
static void roll_about_middle(tr_runner_draw_t *r, float a)
{
	float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, c[3], sy, cy, sp, cp, front = -1e30f;

	for (int p = 0; p < TR_RIG_DRAWN; p++) {
		for (uint16_t v = 0; v < r->mesh[p].nv; v++) {
			for (int k = 0; k < 3; k++) {
				float q = r->xyz[p][v * 3 + k];

				lo[k] = q < lo[k] ? q : lo[k];
				hi[k] = q > hi[k] ? q : hi[k];
			}
		}
	}
	for (int k = 0; k < 3; k++) {
		c[k] = 0.5f * (lo[k] + hi[k]);
	}
	c[0] = 0.0f; /* keep it in its lane */
	tr_sincosf(r->inst.yaw, &sy, &cy);
	tr_sincosf(a, &sp, &cp);
	for (int p = 0; p < TR_RIG_DRAWN; p++) {
		for (uint16_t v = 0; v < r->mesh[p].nv; v++) {
			float *q = &r->xyz[p][v * 3];
			float  z;

			q[0] -= c[0], q[1] -= c[1], q[2] -= c[2];
			/* tr_r3d_emit_mesh()'s pitch + yaw, hold_front()'s formula: the
			 * yaw term matters here too, or front undercounts a leaning
			 * pose and hold_front() below finds more to pull back later. */
			z     = -q[0] * sy + (q[1] * r->inst.scale_y * sp + q[2] * cp) * cy;
			front = z > front ? z : front;
		}
	}
	r->inst.pos.y += c[1] * r->inst.scale_y;
	r->inst.pos.x += c[2] * sy;
	r->inst.pos.z += c[2] * cy;
	r->inst.pitch = a;
	front += r->inst.pos.z - RUNNER_Z;
	if (front > TR_RUNNER_FRONT_Z) {
		r->inst.pos.z -= front - TR_RUNNER_FRONT_Z;
	}
}

/* A spin or a reaction pose can reach further forward than the run: hold
 * the front (yaw and pitch applied) inside TR_RUNNER_FRONT_Z, as
 * roll_about_middle() does for the roll, so an obstacle held at the
 * contact depth never goes into it. */
static void hold_front(tr_runner_draw_t *r)
{
	float sy, cy, sp = 0.0f, cp = 1.0f, front = -1e30f;

	tr_sincosf(r->inst.yaw, &sy, &cy);
	if (r->inst.pitch != 0.0f) {
		tr_sincosf(r->inst.pitch, &sp, &cp);
	}
	for (int p = 0; p < TR_RIG_DRAWN; p++) {
		for (uint16_t v = 0; v < r->mesh[p].nv; v++) {
			const float *q = &r->xyz[p][v * 3];
			float        z = -q[0] * sy + (q[1] * r->inst.scale_y * sp + q[2] * cp) * cy;

			front = z > front ? z : front;
		}
	}
	front += r->inst.pos.z - RUNNER_Z;
	if (front > TR_RUNNER_FRONT_Z) {
		r->inst.pos.z -= front - TR_RUNNER_FRONT_Z;
		tr_front_pullback += front - TR_RUNNER_FRONT_Z;
	}
}

/*
 * The scarf as drawn (P16): the simulated chain moved by however far the
 * drawn neck (the anchor vertex of the skinned body, through the final
 * instance: a roll, a crash, the board lift) is from the one it was
 * stepped against, then a strip along it -- as wide as the character's
 * scarf across the runner's own x, tapering to 65 % -- in world space, in
 * both windings (the rasterizer keeps the face toward the camera), every
 * point on or above the board and inside the contact bound.
 */
static void scarf_draw(const tr_scene_t *s, tr_runner_draw_t *r)
{
	const tr_rig_char_t *c = tr_rig_char(s->chr);
	int                  n = s->scarf_n;
	float                a[3], d[3], sy, cy;

	r->scarf[0].nv = r->scarf[1].nv = 0;
	r->scarf[0].nt = r->scarf[1].nt = 0;
	r->sinst       = (tr_inst_t){&r->scarf[0], {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, TR_TRI_GOURAUD, 0.0f};
	if (n == 0) {
		return;
	}
	inst_point(&r->inst, &r->xyz[0][c->scarf_v[runner_lod(s)] * 3], a);
	for (int q = 0; q < 3; q++) {
		d[q] = a[q] - s->sc[0][q];
	}
	tr_sincosf(r->inst.yaw, &sy, &cy);
	for (int i = 0; i <= n; i++) {
		float hw = 0.5f * c->scarf[5] * (1.0f - 0.35f * (float)i / (float)n), *L = &r->sxyz[i * 6], *R = L + 3;

		for (int q = 0; q < 3; q++) {
			float p = s->sc[i][q] + d[q];

			L[q] = p, R[q] = p;
		}
		L[0] -= hw * cy, L[2] += hw * sy;
		R[0] += hw * cy, R[2] -= hw * sy;
		L[1] = L[1] < 1.0f ? 1.0f : L[1];
		R[1] = R[1] < 1.0f ? 1.0f : R[1];
		/* the contact bound: mid-spin the scarf swings round ahead of the
		 * runner, and must not poke into an obstacle held at contact depth */
		L[2] = L[2] > RUNNER_Z + TR_RUNNER_FRONT_Z ? RUNNER_Z + TR_RUNNER_FRONT_Z : L[2];
		R[2] = R[2] > RUNNER_Z + TR_RUNNER_FRONT_Z ? RUNNER_Z + TR_RUNNER_FRONT_Z : R[2];
	}
	for (int i = 0; i <= n; i++) { /* vertex normals: across x along, the "top" side */
		const float *L = &r->sxyz[i * 6], *R = L + 3, *A = &r->sxyz[(i < n ? i : i - 1) * 6];
		const float *B = &r->sxyz[(i < n ? i + 1 : i) * 6];
		float        u[3] = {R[0] - L[0], R[1] - L[1], R[2] - L[2]}, t[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]};
		float        nn[3] = {u[1] * t[2] - u[2] * t[1], u[2] * t[0] - u[0] * t[2], u[0] * t[1] - u[1] * t[0]};
		float        m = nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2];

		m = m > 1e-9f ? 127.0f / __builtin_sqrtf(m) : 0.0f;
		for (int q = 0; q < 3; q++) {
			int8_t v = (int8_t)(int32_t)(nn[q] * m);

			r->svn[0][i * 6 + q] = r->svn[0][i * 6 + 3 + q] = v;
			r->svn[1][i * 6 + q] = r->svn[1][i * 6 + 3 + q] = (int8_t)-v;
		}
	}
	for (int i = 0; i < n; i++) {
		uint8_t l0 = (uint8_t)(2 * i), r0 = (uint8_t)(2 * i + 1), l1 = (uint8_t)(2 * i + 2), r1 = (uint8_t)(2 * i + 3);
		uint8_t fw[6] = {l0, r0, r1, l0, r1, l1}, bw[6] = {l0, r1, r0, l0, l1, r1};

		memcpy(&r->stri[0][i * 6], fw, 6);
		memcpy(&r->stri[1][i * 6], bw, 6);
		for (int t = 0; t < 2; t++) {
			memcpy(&r->sn[0][(i * 2 + t) * 3], &r->svn[0][i * 6], 3);
			memcpy(&r->sn[1][(i * 2 + t) * 3], &r->svn[1][i * 6], 3);
			r->scol[i * 2 + t] = i == n - 1 || i == 1 ? TR_CHAR_SCARF2 : TR_CHAR_SCARF; /* stripes */
		}
	}
	for (int k = 0; k < 2; k++) {
		r->scarf[k] = (tr_mesh_t){NULL, r->stri[k], r->sn[k], r->scol, (uint16_t)(2 * (n + 1)), (uint16_t)(2 * n),
					  r->svn[k], r->sxyz, c->mesh[0][0]->pal, c->mesh[0][0]->emis};
	}
}

void tr_scene_runner(const tr_scene_t *s, const tr_frame_in_t *in, tr_runner_draw_t *r)
{
	tr_front_pullback = 0.0f;
	r->inst = runner_inst(s, in);
	tr_rig_skin(s->chr, runner_lod(s), s->ch, &s->face, r->xyz, r->vn, r->mesh);
	if (s->roll != 0.0f && !(in->flags & TR_FLAG_CRASH)) {
		/* Duck roll: turn the curled body about its own middle, pitched
		 * forward; on_board() below sets the ball on the board. */
		roll_about_middle(r, s->roll);
	}
	if (in->flags & TR_FLAG_CRASH) {
		/* Thrown back off the obstacle with a hop, tipping over backwards
		 * with a twist, limbs flung out (the crash pose); any jump height
		 * it had blends out as it lands. The tumble pivots on the
		 * runner's rearmost point (its skinned vertices moved so it sits
		 * at local z 0, the instance moved back as far), so tipping back
		 * lifts every vertex instead of sinking the back of it. */
		float ct = crash_time(in);
		float k = clampf(ct / 16.0f, 0.0f, 1.0f), back = KNOCK_BACK * k * (2.0f - k);
		float f = clampf(ct / 18.0f, 0.0f, 1.0f), hop = ct < 16.0f ? 60.0f * sinf_(PI_F * ct / 16.0f) : 0.0f;
		float zmin = 0.0f;

		if (s->crash_roll != 0.0f) {
			/* Hit mid-roll (ducked into a low part): keep turning about
			 * the ball's middle from where the roll was, easing that
			 * angle out into the tumble -- no pop upright. */
			float tumble = -TUMBLE_RAD * f * (2.0f - f);

			roll_about_middle(r, s->crash_roll * (1.0f - smooth01(ct / 8.0f)) + tumble);
			r->inst.pos.y += s->lift * (1.0f - k) + hop - s->lift;
			r->inst.pos.z -= back;
			r->inst.yaw += 0.6f * f;
			r->inst.mesh  = &r->mesh[0];
			r->board_lift = on_board(&r->inst, &r->mesh[1], TR_RIG_DRAWN - 1);
			scarf_draw(s, r);
			return;
		}

		for (int p = 0; p < TR_RIG_DRAWN; p++) {
			for (uint16_t v = 0; v < r->mesh[p].nv; v++) {
				zmin = r->xyz[p][v * 3 + 2] < zmin ? r->xyz[p][v * 3 + 2] : zmin;
			}
		}
		for (int p = 0; p < TR_RIG_DRAWN; p++) {
			for (uint16_t v = 0; v < r->mesh[p].nv; v++) {
				r->xyz[p][v * 3 + 2] -= zmin;
			}
		}
		r->inst.pos   = (tr_v3_t){s->runner_x, s->lift * (1.0f - k) + hop, RUNNER_Z - back + zmin};
		r->inst.yaw   = 0.6f * f;
		r->inst.pitch = -TUMBLE_RAD * f * (2.0f - f);
		if (in->crash_kind == TR_CRASH_KIND_WIRE && ct < JOLT_FRAMES) {
			/* Electrocuted: a fast shudder in x and yaw, fading. */
			float j = 1.0f - ct / JOLT_FRAMES;

			r->inst.pos.x += 9.0f * j * sinf_(ct * 11.3f);
			r->inst.yaw += 0.25f * j * sinf_(ct * 8.7f + 1.0f);
		}
	}
	/* One instance for every part, lifted by their joint lowest vertex. */
	r->inst.mesh = &r->mesh[0];
	r->board_lift = on_board(&r->inst, &r->mesh[1], TR_RIG_DRAWN - 1);
	if (!(in->flags & TR_FLAG_CRASH)) { /* every pose, spin and idle: the contact bound -- a no-op for the
					      * roll now that roll_about_middle() above already measures its
					      * front correctly; still real work for a spin or a reaction that
					      * swings a limb past the bound (hold_front()'s own doc comment) */
		hold_front(r);
	}
	scarf_draw(s, r);
}

/* Camera shake as a screen-space jolt of the projection centre: the same
 * picture moved, so it costs no raster time (a rolled or moved eye
 * measured +0.04 to +0.10x bin+raster: long tris re-slanted across more
 * rows). The amplitude: the packet's (TR_FLAG_SHAKE, a booth HE), else
 * the old HE's crash fade, 14 px over ~0.5 s of crash time. */
void tr_scene_shake(const tr_frame_in_t *in, float *dx, float *dy)
{
	int   crash = (in->flags & TR_FLAG_CRASH) != 0;
	float t     = crash ? crash_time(in) : (float)in->tick + phase_f(in), a;

	if (in->flags & TR_FLAG_SHAKE) {
		a = TR_SCENE_SHAKE_PX * (float)in->shake / 255.0f;
	} else if (crash) {
		a = 14.0f * clampf(1.0f - t / 20.0f, 0.0f, 1.0f);
	} else {
		a = 0.0f;
	}
	*dx = a * sinf_(t * 2.7f);
	*dy = 0.7f * a * sinf_(t * 3.9f + 1.0f);
}

void tr_scene_build(const tr_scene_t *s, const tr_frame_in_t *in, tr_cam_t *cam, tr_dl_t *dl)
{
	tr_dl_dropped = 0;
	tr_scene_build_part(s, in, cam, dl, 0);
}

void tr_scene_build_part(const tr_scene_t *s, const tr_frame_in_t *in, tr_cam_t *cam, tr_dl_t *dl, int part)
{
	int      crash = (in->flags & TR_FLAG_CRASH) != 0;
	float    ct    = crash ? crash_time(in) : 0.0f;
	float    kick  = 40.0f * clampf((float)in->score / 1000.0f, 0.0f, 1.0f);
	tr_v3_t  eye   = {s->cam_x, TR_CAM_EYE_H + 0.45f * s->lift + s->cam_bob + s->dip, RUNNER_Z - TR_CAM_BACK};
	uint32_t S     = scroll_units(in->tick, phase_q16(in));

	dl->n = 0;
	tr_cam_build(cam, eye, 0.0f, TR_CAM_PITCH_DEG * DEG + s->lift * 0.0004f, s->cam_roll, TR_CAM_F_PX + kick);
	{
		float dx, dy;

		tr_scene_shake(in, &dx, &dy);
		cam->cx += dx;
		cam->cy += dy;
	}

	/* The zones this frame (P15): lights, the gate, the look's blend. */
	zctx_t zc;

	zone_ctx(in, &zc);
	/* Far layer: unlit (ambient 1), 40-50 % haze toward the glow. */
	tr_light_t lf = {{0.0f, 0.0f, -1.0f}, 1.0f, {zc.glow[0], zc.glow[1], zc.glow[2]}, 0.0f, 105000.0f, NULL, 0.0f, 0.0f};
	/* The sun burns through the haze: ~15 %. */
	tr_light_t ls = {{0.0f, 0.0f, -1.0f}, 1.0f, {zc.glow[0], zc.glow[1], zc.glow[2]}, 0.0f, 400000.0f, NULL, 0.0f, 0.0f};

	/* The ground: textured near (fogged per sub-span), then the far road
	 * out to GROUND_END (far_road()). */
	if (part != 2) {
		/* Tile boundaries sit at k * TR_TILE_LEN - 2 tiles - ph; the
		 * textured run ends at the far edge of the last tile starting
		 * before TEX_Z (the far road starts at that same edge). */
		int32_t  ph  = (int32_t)(S % TR_TILE_LEN), zb = -2 * TR_TILE_LEN - ph;
		uint32_t j   = (uint32_t)(EYE_Z - zb) / NEAR_GRID;
		uint32_t nt  = (uint32_t)((int32_t)TEX_Z - zb + TR_TILE_LEN - 1) / TR_TILE_LEN;

		/* Two runs, split at tile boundary NEAR_SPLIT: a quad's far
		 * corners are snapped to 1/16 px where a pixel spans many texels,
		 * and over one run the whole depth that error reached the near
		 * rows (measured: texel error back to the per-tile quads' with one
		 * split). v counts from the repeat the run starts in: small
		 * numbers keep persp()'s 1/w product precise. */
		bind_ground(&zc); /* this core only: part 2 never reads the ground slots */
		board_near(dl, cam, &zc, zb, (float)(zb + (int32_t)j * NEAR_GRID), j / 16u, nt); /* first: under */
		ground_near(dl, cam, &zc, zb, (float)(zb + (int32_t)j * NEAR_GRID), (float)(zb + NEAR_SPLIT * TR_TILE_LEN),
			    (uint16_t)((j & 15u) * 0x100u), (uint16_t)((NEAR_SPLIT - j / 16u) * 0x1000u));
		ground_near(dl, cam, &zc, zb, (float)(zb + NEAR_SPLIT * TR_TILE_LEN),
			    (float)(zb + (int32_t)nt * TR_TILE_LEN + NEAR_GRID), 0,
			    (uint16_t)((nt - NEAR_SPLIT) * 0x1000u + 0x100u));
		far_road(dl, cam, &zc, (float)(zb + (int32_t)nt * TR_TILE_LEN));
	}
	for (uint32_t k = 0; part != 2 && k < TR_SCENE_SPLIT_TILE; k++) { /* part 1's walls; part 2's go last (below) */
		float z0 = (float)(int32_t)(k * TR_TILE_LEN) - 2.0f * TR_TILE_LEN - (float)(S % TR_TILE_LEN);

		if (z0 + (float)TR_TILE_LEN < eye.z + TR_CAM_Z_NEAR) {
			continue; /* wholly behind the near plane */
		}
		if (z0 >= GROUND_END) {
			break;
		}
		{
			int     b    = beyond(&zc, z0 + 0.5f * (float)TR_TILE_LEN);
			uint8_t zone = b ? zc.far : zc.near;

			if (zone == TR_ZONE_BOARD) {
				walls(dl, cam, &zc.l[b], S / TR_TILE_LEN + k, z0, s->quality, in);
			} else {
				zone_walls(dl, cam, &zc.l[b], zone, S / TR_TILE_LEN + k, z0, s->quality, in);
			}
		}
	}
	if (part == 1) {
		return; /* the rest is part 2's */
	}

	float tf = (float)in->tick + phase_f(in);

	for (int i = 0; i < 16; i++) {
		const tr_pkt_ent_t *e   = &in->ents[i];
		float               z   = tr_scene_ent_z(in, i);
		float               x   = lane_x(e->lane);
		int                 hit = crash && i == in->crash_ent;

		if (e->kind == 0 || z < eye.z) {
			continue;
		}
		int               lo = z > ((s->quality & TR_LOD_NEAR) ? TR_LOD_NEAR_Z : LOD_Z);
		float             vz = view_z(cam, x, z);
		const tr_light_t *lg = &zc.lg[beyond(&zc, z)];
		ent_fog_t         ef = ent_fog(&zc.le, x, z, vz, e->kind, glow_fog(lg, pal_of(lg)[BOARD_IDX], vz, 1.0f));

		if (ef.k <= 0.0f) {
			continue; /* its spawn step: not grown in yet */
		}

		if (e->kind == 3) {
			live_wire(dl, cam, &ef, s, in, i, x, z, lo, hit);
		} else if (e->kind == 2) {
			ent_emit(dl, cam, &ef, lo ? &tr_mesh_via_lo : &tr_mesh_via, x, 72.0f + 10.0f * sinf_(tf * 0.3f + (float)i), z,
				 tf * 0.12f + (float)i);
		} else if (hit) {
			/* Struck: knocked over away from the runner and crushed. */
			float     k  = clampf((ct - 2.0f) / 10.0f, 0.0f, 1.0f);
			tr_inst_t ob = {!e->low ? &tr_mesh_arch : &tr_mesh_resistor, {x, 0, z}, 0.25f * k, 1.0f - 0.45f * k,
					TR_TRI_GOURAUD, 0.7f * k};

			on_board(&ob, NULL, 0);
			tr_r3d_emit_mesh(dl, cam, &ef.l, &ob);
		} else {
			if (z > ENT_FAR_Z) { /* the arch's posts and lintel, the resistor's body and legs */
				ent_quads(dl, cam, &ef, x, z, !e->low ? tr_far_arch : tr_far_resistor, 3);
			} else {
				ent_emit(dl, cam, &ef, !e->low ? &tr_mesh_arch : lo ? &tr_mesh_resistor_lo : &tr_mesh_resistor, x, 0, z, 0);
			}
		}
	}

	/* The runner, posed by s->ch (runner_anim()), skinned and placed. */
	{
		tr_runner_draw_t r;

		tr_scene_runner(s, in, &r);
		for (int piece = 0; piece < TR_RIG_DRAWN; piece++) { /* body, head, limbs, eyes */
			r.inst.mesh = &r.mesh[piece];
			tr_r3d_emit_mesh(dl, cam, &zc.lr, &r.inst);
		}
		for (int k = 0; k < 2; k++) { /* the scarf, both windings */
			r.sinst.mesh = &r.scarf[k];
			if (r.scarf[k].nt) {
				tr_r3d_emit_mesh(dl, cam, &zc.lr, &r.sinst);
			}
		}
	}

	for (int i = 0; i < TR_PARTICLES; i++) {
		const tr_particle_t *p = &s->p[i];
		float                r = tr_scene_particle_r(p);

		/* Cut at the board (y = 0): the ground is NOZ now, so it no
		 * longer hides the part of a falling particle or shard below it. */
		float lo = p->pos.y - r > 0.0f ? p->pos.y - r : 0.0f;

		if (r > 0.0f && p->pos.y + r > 0.0f) {
			tr_v3_t q[4] = {{p->pos.x - r, lo, p->pos.z}, {p->pos.x - r, p->pos.y + r, p->pos.z},
					{p->pos.x + r, p->pos.y + r, p->pos.z}, {p->pos.x + r, lo, p->pos.z}};

			tr_r3d_emit_quad(dl, cam, q, tr_r3d_palette[p->col & 0xFu], 0);
		}
	}

	gate(dl, cam, &zc, in, eye.z);

	/* Far layer rides with the camera in z (never approaches); x is world
	 * fixed, so a lane change parallaxes it a few px. Over a zone blend the
	 * outgoing sun / moon and skyline melt into the haze as the incoming
	 * ones come out of it. */
	for (int k = 0; k < 2 && (k == 0 || zc.far != zc.near); k++) {
		uint8_t  zn = k ? zc.far : zc.near;
		uint32_t a  = q8(k ? 1.0f - zc.t : zc.t);
		uint16_t n0 = dl->n;

		if (a >= 256u) {
			continue;
		}
		ls.pal = lf.pal = zc.l[k].pal; /* k: near 0, far 1 -- as zc's lights */
		emit(dl, cam, &ls, zlook[zn].sun, SUN_X, SUN_Y, eye.z + SUN_DZ, 0, 1.0f);
		emit(dl, cam, &lf, zlook[zn].sky, 0, 0, eye.z + TR_SCENE_SKY_DZ, 0, 1.0f);
		if (a != 0u) {
			fade_tris(dl, n0, RGB565(zc.glow[0], zc.glow[1], zc.glow[2]), a);
		}
	}
	/* Part 2's walls, the far ones, last (but for a crash's flash border):
	 * were a band's bin ever to fill (TR_BIN_MAX), what it drops are its
	 * last triangles -- a far billboard, not the skyline or the runner. */
	for (uint32_t k = TR_SCENE_SPLIT_TILE; k < TR_TILES; k++) {
		float z0 = (float)(int32_t)(k * TR_TILE_LEN) - 2.0f * TR_TILE_LEN - (float)(S % TR_TILE_LEN);

		if (z0 + (float)TR_TILE_LEN < eye.z + TR_CAM_Z_NEAR) {
			continue; /* wholly behind the near plane */
		}
		if (z0 >= GROUND_END) {
			break;
		}
		{
			int     b    = beyond(&zc, z0 + 0.5f * (float)TR_TILE_LEN);
			uint8_t zone = b ? zc.far : zc.near;

			if (zone == TR_ZONE_BOARD) {
				walls(dl, cam, &zc.l[b], S / TR_TILE_LEN + k, z0, s->quality, in);
			} else {
				zone_walls(dl, cam, &zc.l[b], zone, S / TR_TILE_LEN + k, z0, s->quality, in);
			}
		}
	}
	if (crash) {
		flash_border(dl, ct, flash_rgb(in));
	}
}

void tr_scene_bg(const tr_frame_in_t *in, const tr_cam_t *cam, tr_bg_t *bg)
{
	/* Horizon = the vanishing point of world +z: camera-space direction
	 * (m[0][2], m[1][2], m[2][2]); the band background is flat, so the
	 * row at the screen centre stands for a banked horizon. */
	float hy = cam->cy - cam->f_px * cam->view.m[1][2] / cam->view.m[2][2];

	tr_sv_t sun;
	float   vz;

	/* The zones' looks, blended over a gate's pass (tr_scene_zone()). */
	uint8_t        zn, zf;
	float          gz, t  = tr_scene_zone(in, &zn, &zf, &gz);
	uint32_t       a      = q8(t);
	const zlook_t *la     = &zlook[zn], *lb = &zlook[zf];
	uint8_t        g[3];

	for (int c = 0; c < 3; c++) {
		g[c] = (uint8_t)((float)la->glow[c] + ((float)lb->glow[c] - (float)la->glow[c]) * t + 0.5f);
	}
	*bg         = (tr_bg_t){0};
	bg->horizon = (int32_t)clampf(hy, 0.0f, (float)TR_VIEW_H);
	bg->top     = mix565(la->top, lb->top, a);
	bg->mid     = mix565(la->mid, lb->mid, a);
	bg->mid_q8  = (uint8_t)((float)la->mid_q8 + ((float)lb->mid_q8 - (float)la->mid_q8) * t + 0.5f);
	bg->bot     = RGB565(g[0], g[1], g[2]);
	bg->ground  = bg->bot;
	bg->fx      = TR_BG_DITHER | (la->stars || lb->stars ? TR_BG_STARS : 0u);
	/* stars fade with the blend: (256 - star_dim) / 256 strength */
	bg->star_dim = (uint8_t)(255.0f * (1.0f - ((float)la->stars + ((float)lb->stars - (float)la->stars) * t)) + 0.5f);
	if (tr_r3d_project(cam, (tr_v3_t){SUN_X, SUN_Y, (float)EYE_Z + SUN_DZ}, &sun, &vz)) {
		float r = la->halo_r + (lb->halo_r - la->halo_r) * t;

		bg->sun_x  = (int16_t)clampf((float)(sun.x >> TR_R3D_SUB), -2048.0f, 2047.0f);
		bg->sun_y  = (int16_t)clampf((float)(sun.y >> TR_R3D_SUB), -2048.0f, 2047.0f);
		bg->halo_r = (uint16_t)clampf(r * cam->f_px / vz, 0.0f, 1023.0f);
		bg->halo   = mix565(la->halo, lb->halo, a);
	}
}

void tr_scene_bg_flash(const tr_frame_in_t *in, tr_bg_t *bg)
{
	if (!(in->flags & TR_FLAG_CRASH)) {
		return;
	}
	uint32_t a = (uint32_t)(170.0f * flash_amount(crash_time(in)));

	bg->top    = mix565(bg->top, flash_rgb(in), a);
	bg->mid    = mix565(bg->mid, flash_rgb(in), a);
	bg->halo   = mix565(bg->halo, flash_rgb(in), a);
	bg->star_dim = (uint8_t)(a + a / 2 > bg->star_dim ? a + a / 2 : bg->star_dim); /* <= 255: stars nearly out at the hit */
	bg->bot    = mix565(bg->bot, flash_rgb(in), a);
	bg->ground = mix565(bg->ground, flash_rgb(in), a);
}
