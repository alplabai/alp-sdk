/* tests/host/test_r3d_scene.c -- T5/T6 scene: budget, every entity emits,
 * determinism, and a golden full frame rendered the way the A32 does it
 * (tr_bin_build + 40 x tr_raster_band). TR_DUMP=1 writes /tmp/tr-scene.ppm,
 * /tmp/tr-scene-{0,1,2}.ppm and the crash frames (the art review);
 * TR_DUMP=/dir/prefix- writes /dir/prefix-scene.ppm etc. instead. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../src/render/meshes.h" /* mesh depth extents for the contact check */
#include "../../src/render/proj.h"
#include "../../src/render/r3d_scene.h"
#include "tr_scene_golden.h"

#define PI_T 3.14159265f

#define W TR_R3D_W
#define H TR_R3D_H

static uint16_t       fb[W * H];
static uint16_t       zb[W * TR_BAND_H], cb[W * TR_BAND_H];
static uint16_t       bins[TR_BANDS][TR_BIN_MAX];
static uint32_t       counts[TR_BANDS];
static tr_tri_setup_t setup[TR_DL_MAX_TRIS];
static tr_dl_t        dl, dl2;

static uint32_t crc32(const void *p, size_t n)
{
	const uint8_t *b = p;
	uint32_t       c = 0xFFFFFFFFu;

	for (size_t i = 0; i < n; i++) {
		c ^= b[i];
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
		}
	}
	return ~c;
}

static uint32_t render(const tr_cam_t *cam, const tr_frame_in_t *in)
{
	uint32_t overflow = 0, worst = 0;
	tr_bg_t  bg;

	tr_scene_bg(in, cam, &bg);
	tr_scene_bg_flash(in, &bg);
	tr_bin_build(&dl, setup, bins, counts, &overflow);
	for (int b = 0; b < TR_BANDS; b++) {
		worst = counts[b] > worst ? counts[b] : worst;
		/* fix round 8: clip the last band to TR_VIEW_H, matching
		 * a32/renderer/render.c's own render_band() (r3d.h TR_VIEW_H) --
		 * REQUIRED, not optional: bins[TR_BANDS][...] is sized to the
		 * shrunk TR_BANDS, a span reaching past it corrupts memory. */
		int y_hi = (b + 1) * TR_BAND_H < TR_VIEW_H ? (b + 1) * TR_BAND_H : TR_VIEW_H;

		tr_raster_band(fb, W, b * TR_BAND_H, y_hi, zb, cb, &bg, &dl, setup, bins[b], counts[b]);
	}
	assert(overflow == 0);
	return worst;
}

/* Writes /tmp/tr-<name>.ppm, or <TR_DUMP><name>.ppm when TR_DUMP is an
 * absolute path prefix (a private dump directory). */
static void dump(const char *name)
{
	const char *pre = getenv("TR_DUMP");
	char        path[256];

	snprintf(path, sizeof(path), "%s%s.ppm", pre && pre[0] == '/' ? pre : "/tmp/tr-", name);

	FILE *f = fopen(path, "wb");

	assert(f);
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; i++) {
		uint16_t p      = fb[i];
		uint8_t  rgb[3] = {(uint8_t)((p >> 11) << 3), (uint8_t)(((p >> 5) & 63) << 2), (uint8_t)((p & 31) << 3)};

		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

/* Largest local z of a mesh (its front, toward the approaching world). */
static float mesh_zmax(const tr_mesh_t *m)
{
	int16_t z = m->v[2];

	for (int i = 1; i < m->nv; i++) {
		z = m->v[i * 3 + 2] > z ? m->v[i * 3 + 2] : z;
	}
	return (float)z;
}

/* --- the runner rig (P3b) --- */
static float     rxyz[TR_RIG_DRAWN][TR_RIG_MAX_V * 3];
static int8_t    rvn[TR_RIG_DRAWN][TR_RIG_MAX_V * 3];
static tr_mesh_t rmesh[TR_RIG_DRAWN];
static int       rchr; /* the character under test (P16) */
static int       rlod; /* ... and its level of detail (P16b) */

static void skin(const float *ch)
{
	tr_rig_skin(rchr, rlod, ch, NULL, rxyz, rvn, rmesh);
}

/* Lowest skinned y over both meshes; foot >= 0: only that side's shin +
 * foot (the ankle ring and the shoe); *at: its vertex (mesh * 256 + i). */
static float rig_low(int foot, int *at)
{
	float lo = 1e30f;

	if (at) {
		*at = 0; /* defined even if no vertex matched (-O3 sees that path) */
	}

	for (int p = 0; p < TR_RIG_DRAWN; p++) {
		const uint8_t *bn = tr_rig_bones(rchr, rlod, p);

		for (int i = 0; i < rmesh[p].nv; i++) {
			int b = bn[i];

			if (foot == 0 && b != TR_RIG_SHIN0 && b != TR_RIG_FOOT0) {
				continue;
			}
			if (foot == 1 && b != TR_RIG_SHIN1 && b != TR_RIG_FOOT1) {
				continue;
			}
			if (rxyz[p][i * 3 + 1] < lo) {
				lo = rxyz[p][i * 3 + 1];
				if (at) {
					*at = p * 256 + i;
				}
			}
		}
	}
	return lo;
}

static float rig_front(void)
{
	float z = -1e30f;

	for (int p = 0; p < TR_RIG_DRAWN; p++) {
		for (int i = 0; i < rmesh[p].nv; i++) {
			z = rxyz[p][i * 3 + 2] > z ? rxyz[p][i * 3 + 2] : z;
		}
	}
	return z;
}

static float rig_top(void)
{
	float y = -1e30f;

	for (int p = 0; p < TR_RIG_DRAWN; p++) {
		for (int i = 0; i < rmesh[p].nv; i++) {
			y = rxyz[p][i * 3 + 1] > y ? rxyz[p][i * 3 + 1] : y;
		}
	}
	return y;
}

/* Joints stay closed: every vertex turns rigidly about its bone's joint,
 * and that joint is where the PARENT bone carries it -- so a child cannot
 * drift off its parent. Worst change of a vertex's distance from its
 * joint, bind vs posed (mid-joint bones, scaled by design, excepted). */
static float rig_gap(const float *ch)
{
	float worst = 0.0f;

	for (int p = 0; p < TR_RIG_PARTS; p++) {
		const tr_mesh_t *m  = tr_rig_chars[rchr].mesh[rlod][p];
		const uint8_t   *bn = tr_rig_bones(rchr, rlod, p);

		for (int i = 0; i < m->nv; i++) {
			const tr_rig_bone_t *d = &tr_rig_chars[rchr].bone[bn[i]];
			float                j[3], d0 = 0.0f, d1 = 0.0f, bj[3] = {d->jx, d->jy, d->jz};

			if (d->sc_ch >= 0) {
				continue;
			}
			tr_rig_joint(rchr, ch, bn[i], j);
			for (int k = 0; k < 3; k++) {
				float u = (float)m->v[i * 3 + k] - bj[k], w = rxyz[p][i * 3 + k] - j[k];

				d0 += u * u, d1 += w * w;
			}
			float g = d1 > d0 ? d1 - d0 : d0 - d1; /* |d1^2 - d0^2| */

			worst = g > worst ? g : worst;
		}
	}
	return worst;
}

/* Worst stretch of a triangle edge joining two bones, skinned length over
 * bind length, squared (a joint pulled open shows here). */
static float rig_stretch(void)
{
	float worst = 1.0f;

	for (int p = 0; p < TR_RIG_PARTS; p++) {
		const tr_mesh_t *m  = tr_rig_chars[rchr].mesh[rlod][p];
		const uint8_t   *bn = tr_rig_bones(rchr, rlod, p);

		for (int t = 0; t < m->nt; t++) {
			for (int e = 0; e < 3; e++) {
				int   a = m->tri[t * 3 + e], b = m->tri[t * 3 + (e + 1) % 3];
				float d0 = 0.0f, d1 = 0.0f;

				if (bn[a] == bn[b]) {
					continue;
				}
				for (int k = 0; k < 3; k++) {
					float u = (float)(m->v[a * 3 + k] - m->v[b * 3 + k]);
					float w = rxyz[p][a * 3 + k] - rxyz[p][b * 3 + k];

					d0 += u * u, d1 += w * w;
				}
				if (d0 > 4.0f) { /* ignore sub-2-unit edges */
					float r = d1 / d0; /* squared ratio; the inside of a bend
							    * folds (compresses) by design */

					worst = r > worst ? r : worst;
				}
			}
		}
	}
	return worst;
}

/* Runner draw helpers (tr_scene_runner): local extremes of the skinned
 * meshes, before the instance. */
static tr_runner_draw_t rd;

/* World top (y) and front (z past TR_PROJ_Z_RUNNER) of the runner as drawn:
 * scale_y, the instance pitch (roll / tumble) and position applied, yaw
 * (the small lane lean) left out. */
static void runner_world(const tr_runner_draw_t *r, float *top, float *front)
{
	float sp = 0.0f, cp = 1.0f;

	tr_sincosf(r->inst.pitch, &sp, &cp);
	*top = *front = -1e30f;
	for (int p = 0; p < TR_RIG_DRAWN; p++) {
		for (int i = 0; i < r->mesh[p].nv; i++) {
			float ly = r->xyz[p][i * 3 + 1] * r->inst.scale_y, lz = r->xyz[p][i * 3 + 2];
			float y = ly * cp - lz * sp + r->inst.pos.y, z = ly * sp + lz * cp + r->inst.pos.z - (float)TR_PROJ_Z_RUNNER;

			*top   = y > *top ? y : *top;
			*front = z > *front ? z : *front;
		}
	}
}

static float runner_top(const tr_runner_draw_t *r)
{
	float t, f;

	runner_world(r, &t, &f);
	return t;
}

static float runner_z(const tr_runner_draw_t *r, int far)
{
	float z = far ? -1e30f : 1e30f;

	for (int p = 0; p < TR_RIG_DRAWN; p++) {
		for (int i = 0; i < r->mesh[p].nv; i++) {
			float v = r->xyz[p][i * 3 + 2];

			z = far ? (v > z ? v : z) : (v < z ? v : z);
		}
	}
	return z;
}

/* A crash frame: the golden run, the runner (lane 1) hit by slot 4 -- a
 * low obstacle (or high) moved into lane 1 at y 1110, inside step.c's
 * collision band [1104, 1122) -- crash_tick `ct`. low 2 / 3: a high / low
 * live wire (P4b) instead. */
static tr_frame_in_t crash_in(uint8_t ct, int low)
{
	tr_frame_in_t in   = tr_scene_golden_in(1234, 1);
	int           wire = low >> 1;

	low &= 1;
	in.ents[4]    = (tr_pkt_ent_t){(uint8_t)(wire ? 3 : 1), 1, (uint8_t)low, 0, 1110, 0};
	in.flags      = TR_FLAG_CRASH;
	in.crash_tick = ct;
	in.crash_ent  = 4;
	in.crash_lane = 1;
	in.crash_kind = wire ? TR_CRASH_KIND_WIRE : low ? TR_CRASH_KIND_LOW : TR_CRASH_KIND_HIGH;
	return in;
}

/* The golden run with every obstacle a live wire (P4b), heights kept. */
static tr_frame_in_t wires_in(uint32_t tick)
{
	tr_frame_in_t in = tr_scene_golden_in(tick, 1);

	for (int i = 0; i < 16; i++) {
		in.ents[i].kind = in.ents[i].kind == 1 ? 3 : in.ents[i].kind;
	}
	return in;
}

/* A fresh scene through a few live frames and then the crash up to `ct`,
 * built at `ct`. */
static void crash_run(tr_scene_t *s, uint8_t ct, int low, tr_cam_t *cam, tr_dl_t *out)
{
	tr_frame_in_t a = tr_scene_golden_in(1230, 1);

	tr_scene_init(s);
	for (int k = 0; k < 4; k++, a.tick++) {
		tr_scene_step(s, &a);
	}
	for (int c = 0; c <= ct; c++) {
		tr_frame_in_t in = crash_in((uint8_t)c, low);

		tr_scene_step(s, &in);
	}
	tr_frame_in_t in = crash_in(ct, low);

	tr_scene_build(s, &in, cam, out);
}

/* Lowest world y of any DL vertex nearer than view z 10000 (skips the far
 * sun/skyline layer), back-projected through `cam`: w -> view z (with
 * TR_TRI_UVX8's fraction bits), 28.4 screen -> camera x/y, then the
 * transpose of the camera rotation. Vertices at w 0xFFFF (the near plane,
 * the flash-border overlay) are skipped. Good to about +-1 world unit. */
static float min_world_y(const tr_dl_t *d, const tr_cam_t *cam)
{
	const float (*m)[4] = cam->view.m;
	float       lo      = 1e30f;

	for (uint16_t i = 0; i < d->n; i++) {
		for (int k = 0; k < 3; k++) {
			const tr_tri_t *t = &d->tri[i];
			float           w = (float)t->a[k].w + ((t->flags & TR_TRI_UVX8) ? (float)t->a[k].rgb / 65536.0f : 0.0f);

			if (t->a[k].w == 0xFFFF || w < 65535.0f * TR_CAM_Z_NEAR / 10000.0f) {
				continue;
			}
			float z = 65535.0f * TR_CAM_Z_NEAR / w;
			float x = ((float)t->v[k].x / 16.0f - cam->cx) * z / cam->f_px;
			float y = -((float)t->v[k].y / 16.0f - cam->cy) * z / cam->f_px;
			float wy = m[0][1] * (x - m[0][3]) + m[1][1] * (y - m[1][3]) + m[2][1] * (z - m[2][3]);

			lo = wy < lo ? wy : lo;
		}
	}
	return lo;
}

/* Foot lock (P3d): over `n` phases of the run cycle, a foot's lowest
 * vertex while it is on the board (the same vertex as the sample
 * before) against the board under it, which moves -`v` units a tick in z.
 * Returns the worst |foot velocity - board velocity| (x and z, units a
 * tick); *samples: how many planted samples, *vz: their mean z velocity,
 * *mid: the worst away from touch-down and lift-off (0.01 of the cycle).
 * On the board = lowest vertex under y `on`. */
static float foot_residual(int n, float v, float on, int *samples, float *vz, float *mid)
{
	float ch[TR_ANIM_CH], pp[2][3] = {{0}}, worst = 0.0f, sum = 0.0f;
	int   pat[2] = {-1, -1};

	*samples = 0, *mid = 0.0f;
	for (int k = 0; k <= n; k++) {
		tr_rig_run((float)k / (float)n, ch, 1.0f);
		skin(ch);
		for (int f = 0; f < 2; f++) {
			int          at;
			float        fy = rig_low(f, &at);
			const float *q  = &rxyz[at / 256][(at % 256) * 3];

			if (fy < on && at == pat[f]) {
				float vx = (q[0] - pp[f][0]) * (float)n / (float)TR_ANIM_CYCLE;
				float dz = (q[2] - pp[f][2]) * (float)n / (float)TR_ANIM_CYCLE;
				float r  = sqrtf(vx * vx + (dz + v) * (dz + v));

				float u = (float)k / (float)n + 0.5f * (float)f; /* leg phase, touch-down at 0 */

				u -= (float)(int)u;
				if (u > 0.01f && u < TR_ANIM_STANCE - 0.01f) { /* mid-stance: the lock at full weight */
					*mid = r > *mid ? r : *mid;
				}
				worst = r > worst ? r : worst;
				sum += dz;
				(*samples)++;
			}
			pat[f] = fy < on ? at : -1;
			memcpy(pp[f], q, sizeof(pp[f]));
		}
	}
	*vz = *samples ? sum / (float)*samples : 0.0f;
	return worst;
}

static int live_particles(const tr_scene_t *s)
{
	int n = 0;

	for (int i = 0; i < TR_PARTICLES; i++) {
		n += s->p[i].life > 0.0f;
	}
	return n;
}

/* Frame-rate independence (P3d): the packet the HE sends at real time `t`
 * s on a `hz` panel, for a scripted run -- a lane change at 0.2 s, a jump
 * from 0.5 s landing at 1.1 s, a pickup at 1.5 s, the fatal hit at 2.0 s
 * (crash frames from there, crash_tick in 40 Hz frames as tr_mbox.c
 * publishes it). Every event on a tenth of a second: a frame at both 30
 * and 40 Hz. */
static tr_frame_in_t hz_in(double t, int hz)
{
	tr_frame_in_t in = tr_scene_golden_in(3000, 1);
	double        g  = t * 20.0 + 1e-9; /* game steps at the play pace */
	uint32_t      el = (uint32_t)g;

	in.hz      = (uint8_t)hz;
	in.pace_q8 = (uint8_t)((((TR_GAME_PACE_Q8 << 8) * 40u + (uint32_t)hz / 2u) / (uint32_t)hz) >> 8);
	if (t >= 2.0) { /* crashed: the world frozen at the fatal step */
		in.tick       = 3000 + 40;
		in.flags      = TR_FLAG_CRASH;
		in.crash_tick = (uint8_t)((t - 2.0) * 40.0 + 1e-6);
		in.crash_frac = (uint16_t)(((t - 2.0) * 40.0 + 1e-6 - in.crash_tick) * 65536.0); /* as tr_mbox.c */
		in.crash_ent  = 4, in.crash_lane = in.ents[4].lane, in.crash_kind = TR_CRASH_KIND_LOW;
		in.pace_q8    = 0;
		in.lane       = 2;
		in.score      = 130;
		return in;
	}
	in.tick  = 3000 + el;
	in.phase = (uint16_t)((g - (double)el) * 65536.0);
	in.flags = TR_FLAG_ALIVE | (in.phase ? TR_FLAG_PHASE : 0u);
	in.lane  = t >= 0.2 ? 2 : 1;
	in.score = t >= 1.5 ? 130 : 120;
	if (el >= 10 && el < 22) {
		in.flags |= TR_FLAG_AIRBORNE;
		in.air_ticks = (uint8_t)(TR_AIR_TICKS - (el - 10));
	}
	for (int i = 0; i < 16; i++) {
		in.ents[i].y = (int16_t)(in.ents[i].y + (int)el * TR_SCROLL_PX / 4);
	}
	return in;
}

/* P16: the stream a 30 / 40 Hz HE sends at real time t for a scripted run
 * as character `chr` -- hz_in()'s lane change and jump, the reactions
 * (pickup pump 0.3 s, glance back to the left 0.9 s, near-miss stumble
 * 1.3 s, combo spin 1.8 s), then from P16_IDLE_T the attract lobby (the
 * world frozen, TR_FLAG_IDLE, idle_ms). */
#define P16_IDLE_T 2.8
#define SCARF_RATE_TOL 15.0f /* 30 vs 40 Hz scarf points, units: 8.4 measured; sub-steps ignoring the rate: 29 */
static tr_frame_in_t p16_in(double t, int hz, int chr)
{
	tr_frame_in_t in = hz_in(t < 1.95 ? t : 1.95, hz);
	static const struct {
		double  at;
		uint8_t kind;
		int8_t  side;
	} ev[] = {{0.3, TR_REACT_PICKUP, 1}, {0.9, TR_REACT_PASS, -1}, {1.3, TR_REACT_NEAR, 1}, {1.8, TR_REACT_COMBO, 1}};

	in.flags |= TR_FLAG_CHAR;
	in.character = (uint8_t)chr;
	in.react_ms  = 65535;
	for (int i = 0; i < 4; i++) {
		if (t + 1e-9 >= ev[i].at) {
			in.react      = ev[i].kind;
			in.react_side = (uint8_t)ev[i].side;
			in.react_seq  = (uint8_t)(i + 1);
			in.react_ms   = (uint16_t)((t - ev[i].at) * 1000.0 + 1e-6);
		}
	}
	if (t >= P16_IDLE_T) {
		in.flags   = TR_FLAG_ALIVE | TR_FLAG_CHAR | TR_FLAG_IDLE;
		in.phase   = 0;
		in.idle_ms = (uint16_t)((t - P16_IDLE_T) * 1000.0 + 1e-6);
		in.react   = TR_REACT_NONE;
		memset(in.ents, 0, sizeof(in.ents));
	}
	return in;
}

/* The drawn runner's world front (z past TR_PROJ_Z_RUNNER) and bottom,
 * the instance's yaw and pitch applied. */
static void runner_extent(const tr_runner_draw_t *r, float *front, float *bottom)
{
	float sy, cy, sp, cp;

	tr_sincosf(r->inst.yaw, &sy, &cy);
	tr_sincosf(r->inst.pitch, &sp, &cp);
	*front = -1e30f, *bottom = 1e30f;
	for (int p = 0; p < TR_RIG_DRAWN; p++) {
		for (int i = 0; i < r->mesh[p].nv; i++) {
			float lx = r->xyz[p][i * 3], ly = r->xyz[p][i * 3 + 1] * r->inst.scale_y, lz = r->xyz[p][i * 3 + 2];
			float y = ly * cp - lz * sp + r->inst.pos.y, z = ly * sp + lz * cp;
			float wz = -lx * sy + z * cy + r->inst.pos.z - (float)TR_PROJ_Z_RUNNER;

			*front  = wz > *front ? wz : *front;
			*bottom = y < *bottom ? y : *bottom;
		}
	}
}

/* CPU time, ns: ISO C clock() so the A32 newlib build links too. */
static int64_t now_ns(void)
{
	return (int64_t)clock() * (1000000000 / CLOCKS_PER_SEC);
}

/* --- the living board (P12) --- */
/* Screen rows (clipped) and the union of pixels the DL triangles
 * d->tri[lo..hi) cover: the raster's per-row and per-pixel work proxies. */
static void cover(const tr_dl_t *d, uint16_t lo, uint16_t hi, uint32_t *rows, uint32_t *px)
{
	*rows = 0;
	memset(fb, 0, sizeof(fb));
	for (uint16_t i = lo; i < hi; i++) {
		tr_tri_t t = d->tri[i];
		int      y0 = H, y1 = 0;

		for (int k = 0; k < 3; k++) {
			int y = t.v[k].y >> TR_R3D_SUB;

			y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
		}
		y0 = y0 < 0 ? 0 : y0, y1 = y1 > H ? H : y1;
		*rows += y1 > y0 ? (uint32_t)(y1 - y0) : 0u;
		t.c     = 0xFFFF;
		t.flags = 0;
		tr_raster_tri(fb, W, &t);
	}
	*px = 0;
	for (int i = 0; i < W * H; i++) {
		*px += fb[i] == 0xFFFF;
	}
}

/* Every far quad (tr_far_quad_t, r3d_scene.h) is its mesh's own: each edge
 * on a vertex coordinate of that mesh's triangles of the quad's colour,
 * inside their bounds. */
static void far_quads_match(const char *name, const tr_mesh_t *m, const tr_far_quad_t *q, int n)
{
	for (int i = 0; i < n; i++) {
		float e[4] = {q[i].x0, q[i].x1, q[i].y0, q[i].y1};
		int   hit[4] = {0, 0, 0, 0}, any = 0;
		float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};

		for (int t = 0; t < m->nt; t++) {
			if (m->col[t] != q[i].col) {
				continue;
			}
			any = 1;
			for (int k = 0; k < 3; k++) {
				const int16_t *v = &m->v[m->tri[t * 3 + k] * 3];

				for (int a = 0; a < 2; a++) {
					lo[a] = (float)v[a] < lo[a] ? (float)v[a] : lo[a];
					hi[a] = (float)v[a] > hi[a] ? (float)v[a] : hi[a];
				}
				for (int j = 0; j < 4; j++) {
					hit[j] |= (float)v[j < 2 ? 0 : 1] == e[j];
				}
			}
		}
		printf("far quad %s %d: x %.0f..%.0f y %.0f..%.0f colour %u (mesh colour %u: x %.0f..%.0f y %.0f..%.0f)\n", name, i,
		       (double)q[i].x0, (double)q[i].x1, (double)q[i].y0, (double)q[i].y1, q[i].col, q[i].col, (double)lo[0],
		       (double)hi[0], (double)lo[1], (double)hi[1]);
		assert(any && hit[0] && hit[1] && hit[2] && hit[3]);
		assert(q[i].x0 >= lo[0] && q[i].x1 <= hi[0] && q[i].y0 >= lo[1] && q[i].y1 <= hi[1] && q[i].x0 < q[i].x1 && q[i].y0 < q[i].y1);
	}
}

int main(void)
{
	tr_scene_t    s;
	tr_cam_t      cam;
	tr_frame_in_t in = tr_scene_golden_in(1234, 1);

	setvbuf(stdout, NULL, _IONBF, 0);

	/* 0. The runner rig (P3b), over the run cycle and the key poses -- for
	 * every character (P16: shared legs and cycle, own upper body). */
	uint32_t rig_crc = 0;

	for (int cl = 0; cl < TR_CHARS * TR_RIG_LODS; cl++) {
		float    ch[TR_ANIM_CH], lo_all = 1e30f, front = -1e30f, stretch = 1.0f, gap = 0.0f, duck_top = 0.0f;
		float    top = -1e30f;
		uint32_t crc = 0;
		int      flight = 0, n = 64, nt = 0;

		rchr = cl / TR_RIG_LODS, rlod = cl % TR_RIG_LODS;

		/* a. deterministic: the skinned run cycle at 16 phases. */
		for (int k = 0; k < 16; k++) {
			tr_rig_run((float)k / 16.0f, ch, 1.0f);
			skin(ch);
			crc ^= crc32(rxyz, sizeof(rxyz)) + (uint32_t)k;
		}
		/* b-e over 64 phases a stride; clear: the swinging foot's lowest
		 * point while its lock is off (outside the stance and its
		 * TR_ANIM_LOCK_BLEND easing either side) */
		float clear = 1e30f;

		for (int k = 0; k <= n; k++) {
			tr_rig_run((float)k / (float)n, ch, 1.0f);
			skin(ch);

			float lo = rig_low(-1, NULL);

			lo_all  = lo < lo_all ? lo : lo_all;
			flight += k < n && lo > 2.0f; /* clear air */
			front   = rig_front() > front ? rig_front() : front;
			top     = rig_top() > top ? rig_top() : top;
			stretch = rig_stretch() > stretch ? rig_stretch() : stretch;
			gap     = rig_gap(ch) > gap ? rig_gap(ch) : gap;
			for (int f = 0; f < 2; f++) {
				float u = (float)k / (float)n + 0.5f * (float)f;

				u -= (float)(int)u;
				if (u > TR_ANIM_STANCE + TR_ANIM_LOCK_BLEND && u < 1.0f - TR_ANIM_LOCK_BLEND) {
					clear = rig_low(f, NULL) < clear ? rig_low(f, NULL) : clear;
				}
			}
		}
		float world = (float)tr_scene_scroll(1000, 0) / 1000.0f; /* units a tick */

		/* the key poses: the P3b set and the P16 personality set (drawn
		 * standing, or layered over the run: every one on the board) */
		for (int p = 0; p < TR_ANIM_POSES; p++) {
			skin(tr_rig_pose(p));
			lo_all  = rig_low(-1, NULL) < lo_all ? rig_low(-1, NULL) : lo_all;
			if (p != TR_ANIM_POSE_DUCK && p < TR_ANIM_POSE_STAND) { /* the duck is drawn rolled, the P16 set
										 * held by hold_front(): 6f / 11 measure
										 * them as drawn */
				front = rig_front() > front ? rig_front() : front;
			}
			stretch = rig_stretch() > stretch ? rig_stretch() : stretch;
			gap     = rig_gap(tr_rig_pose(p)) > gap ? rig_gap(tr_rig_pose(p)) : gap;
			if (p == TR_ANIM_POSE_DUCK) {
				duck_top = rig_top();
			}
		}
		for (int p = 0; p < TR_RIG_PARTS; p++) {
			nt += tr_rig_chars[rchr].mesh[rlod][p]->nt, assert(tr_rig_chars[rchr].mesh[rlod][p]->nv <= TR_RIG_MAX_V);
		}
		nt += TR_FACE_T + 2 * (int)tr_rig_chars[rchr].scarf[3]; /* the eyes, the scarf's side facing the camera */
		/* P16b: every mesh's normals unit (int8 127 +- rounding) and welded:
		 * coincident vertices of one bone either share one normal or split
		 * on purpose (>= 30 deg: a box edge, a cap against a side) --
		 * nothing in between, which would shade a smooth surface creased */
		for (int p = 0; p < TR_RIG_PARTS; p++) {
			const tr_mesh_t *m = tr_rig_chars[rchr].mesh[rlod][p];
			const uint8_t   *bn = tr_rig_bones(rchr, rlod, p);

			for (int i = 0; i < m->nv; i++) {
				const int8_t *a = &m->vn[i * 3];
				float         la = sqrtf((float)(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]));

				assert(la > 118.0f && la < 128.0f);
				for (int j = i + 1; j < m->nv; j++) {
					const int8_t *b = &m->vn[j * 3];

					if (bn[i] != bn[j] || memcmp(&m->v[i * 3], &m->v[j * 3], 3 * sizeof(int16_t)) != 0) {
						continue;
					}
					float lb = sqrtf((float)(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]));
					float c  = (float)(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);

					assert(c < 0.866f); /* split by >= 30 deg, or it would have been welded */
				}
			}
		}
		printf("rig %s lod %d: pose crc %08x, %d tris, flight %d/%d phases, lowest y %.2f, swing clearance %.2f, front z "
		       "%.1f, top %.1f, joint gap %.3f, outer-bend stretch x%.2f, duck pose top %.1f\n",
		       tr_rig_chars[rchr].name, rlod, (unsigned)crc, nt, flight, n, (double)lo_all, (double)clear, (double)front,
		       (double)top, (double)gap, (double)stretch, (double)duck_top);
		rig_crc = rig_crc * 31u + crc;
		assert(flight >= 37 && flight <= 43 && n == 64); /* both feet off the board 58-67 % (P3d: 40, 62.5 %) */
		assert(clear > 6.0f); /* a swinging foot never skims the board (7.94) */
		assert(lo_all > -0.25f);                                  /* nothing under the board */
		assert(front <= TR_RUNNER_FRONT_Z);                       /* contact bound (CONTACT_DZ) */
		assert(duck_top < 145.0f);                                /* duck: under the arch bar (150) */
		assert(gap < 0.5f);                                       /* joints stay closed (units^2) */
		assert(stretch < 9.0f);                                   /* outer bends: < 3x edge length */
		assert(nt <= (rlod ? 900 : 1600));                        /* P16b tri caps per character: high / low LOD */
		assert(top > 150.0f && top < 240.0f);                     /* a runner-sized character */

		/* f. foot lock (P3d): a foot on the board moves with the board
		 * under it. Before P3d: the planted feet swept at 31.3 units a
		 * tick against the board's 89.3 (x2.9), worst 68.2 off it. The
		 * board speed the rig was designed for is the scroll's. */
		{
			int   ns, ns2;
			float mid, mid2, vz, vz2, res = foot_residual(1200, world, 0.1f, &ns, &vz, &mid); /* on it (sole at 0.00 high LOD, -0.07 low) */
			float res2 = foot_residual(1200, world, 0.25f, &ns2, &vz2, &mid2); /* + touch-down / lift-off */

			printf("rig %s: foot lock: %d planted samples, foot vz %.3f units/tick vs board %.3f, residual max %.3f "
			       "units/tick (%.1f units/s at 20 steps/s, %.2f %% of the board), mid-stance %.3f; within 0.25 "
			       "of the board (%d samples) %.3f; cadence %.2f steps/s\n",
			       tr_rig_chars[rchr].name, ns, (double)vz, (double)-world, (double)res, (double)(res * 20.0f),
			       (double)(100.0f * res / world), (double)mid, ns2, (double)res2,
			       (double)(2.0f * 20.0f / (float)TR_ANIM_CYCLE));
			assert(TR_ANIM_GROUND_V > world - 0.01f && TR_ANIM_GROUND_V < world + 0.01f);
			assert(ns >= 360);                              /* 2 x 15 % of 1200 phases, + the ease (384) */
			assert(vz > -world - 0.05f && vz < -world + 0.05f); /* with the board, on average (0.033 off) */
			assert(mid < 0.06f && res < 0.6f); /* at every sample: < 0.07 % mid-stance, < 0.7 % at its ends */
			assert(res2 < 1.6f); /* coming down / leaving: in the air, < 1.8 % (1.41) */
		}

		/* cost: every part posed and skinned (host, informational) */
		if (rlod) {
			continue; /* cost: the high LOD (the legs are the same bones) */
		}
		int64_t t0 = now_ns();

		for (int k = 0; k < 2000; k++) {
			tr_rig_run((float)k / 97.0f, ch, 1.0f);
			skin(ch);
		}
		printf("rig %s: pose + skin %d + %d + %d + %d + %d + %d verts: host %lld ns\n", tr_rig_chars[rchr].name,
		       rmesh[0].nv, rmesh[1].nv, rmesh[2].nv, rmesh[3].nv, rmesh[4].nv, rmesh[5].nv,
		       (long long)((now_ns() - t0) / 2000));
	}
	rchr = 0, rlod = 0;
	printf("rig: all characters pose crc %08x\n", (unsigned)rig_crc);
	assert(rig_crc == TR_RIG_RUN_CRC);

	far_quads_match("arch", &tr_mesh_arch, tr_far_arch, 3);
	far_quads_match("resistor", &tr_mesh_resistor_lo, tr_far_resistor, 3);
	far_quads_match("post low", &tr_mesh_post_low_lo, tr_far_post_low, 2);
	far_quads_match("post high", &tr_mesh_post_high_lo, tr_far_post_high, 2);

	/* 1. Budget: the reference frame is rich but inside TR_DL_MAX_TRIS,
	 * nothing dropped, every band's bin fits. */
	tr_scene_init(&s);
	tr_scene_step(&s, &in);
	tr_scene_build(&s, &in, &cam, &dl);
	printf("typical: %u tris, dropped %u\n", dl.n, (unsigned)tr_dl_dropped);
	assert(dl.n >= 1300 && dl.n <= 2700); /* near ground: 20 NOZ tris, not 18 a tile; far scenery billboards to the skyline */
	assert(tr_dl_dropped == 0);

	/* 2. Every entity emits: each live slot alone adds triangles to the
	 * entity-free frame. */
	{
		tr_frame_in_t none = in;

		memset(none.ents, 0, sizeof(none.ents));
		tr_scene_build(&s, &none, &cam, &dl2);
		for (int i = 0; i < 16; i++) {
			tr_frame_in_t one = none;

			if (in.ents[i].kind == 0) {
				continue;
			}
			one.ents[i] = in.ents[i];
			tr_scene_build(&s, &one, &cam, &dl);
			assert(dl.n > dl2.n);
		}
	}

	/* 2b. Parts grow in on the far road: a part on the spawn row
	 * (TR_SPAWN_Y, ~21,700 deep, the far end of the visible road) adds no
	 * triangle -- it grows in from nothing, it does not pop (the screen
	 * side: test_r3d_ent_lead.c) -- and one at y 0 (~9,200) is drawn in its
	 * own colours, not the glow's -- a live wire too. The camera frames the
	 * runner low: feet near 987 px, horizon near 445 px. */
	{
		tr_frame_in_t none = in, one;
		uint16_t      glow = (uint16_t)(((206 >> 3) << 11) | ((112 >> 2) << 5) | (72 >> 3));
		int           ys[4] = {TR_SPAWN_Y, 0, TR_SPAWN_Y, 0}, kinds[4] = {1, 1, 3, 3}; /* a resistor, a live wire */

		memset(none.ents, 0, sizeof(none.ents));
		for (int k = 0; k < 4; k++) {
			int fogged = 0, total;

			tr_scene_build(&s, &none, &cam, &dl2);
			one         = none;
			one.ents[0] = (tr_pkt_ent_t){(uint8_t)kinds[k], 1, 1, 0, (int16_t)ys[k], 0};
			tr_scene_build(&s, &one, &cam, &dl);
			total = dl.n - dl2.n;
			assert((k & 1) ? total > 0 : total == 0); /* spawn row: not grown in yet */
			/* the part's tris sit where the entity loop emits them: before
			 * the runner, i.e. the first `total` tris after the world */
			for (int t = 0; t < dl.n && total > 0; t++) {
				int in2 = t < dl2.n && memcmp(&dl.tri[t], &dl2.tri[t], sizeof(dl.tri[t])) == 0;

				if (!in2) {
					for (int v = 0; v < 3; v++) {
						uint16_t c = dl.tri[t].a[v].rgb;
						int      d = abs((int)(c >> 11) - (glow >> 11)) + abs((int)((c >> 5) & 63) - ((glow >> 5) & 63)) +
							abs((int)(c & 31) - (glow & 31));

						fogged += d <= 2;
					}
					break; /* the first differing tri is the part's */
				}
			}
			assert(fogged < 3); /* y 0: coloured */
		}
		tr_sv_t sv;
		float   vz, hy;

		tr_scene_build(&s, &none, &cam, &dl2);
		hy = cam.cy - cam.f_px * cam.view.m[1][2] / cam.view.m[2][2];
		tr_r3d_project(&cam, (tr_v3_t){s.runner_x, 0, (float)TR_PROJ_Z_RUNNER}, &sv, &vz);
		printf("framing: horizon %.0f px, runner feet %.0f px\n", (double)hy, (double)(sv.y >> 4));
		/* fix round 9: 240..290 / 790..845 -> 145..195 / 681..731 -- the
		 * camera pitch moved again (18 -> 23 deg, r3d_scene.h
		 * TR_CAM_PITCH_DEG): pitching down MORE lifts the runner's fixed-
		 * follow-distance screen position UP-frame (more ground plane
		 * shows between the eye and the runner), the direction that
		 * actually clears the viewport bottom -- confirmed against the
		 * real composited render, not just this collision-line proxy
		 * (see r3d_scene.h's own comment on the two prior, reversed
		 * readings). Measured 170/706 px here at the new pitch. */
		/* Half/half layout: the same bounds, tuned at TR_VIEW_TUNED_H,
		 * rescaled to TR_VIEW_H (r3d.h TR_VIEW_PX) -- the focal length
		 * scales with the viewport, so the framing keeps its proportions
		 * (measured 127/530 px at TR_VIEW_H 640). */
		assert(hy > (float)TR_VIEW_PX(145) && hy < (float)TR_VIEW_PX(195) && (sv.y >> 4) > TR_VIEW_PX(681) &&
		       (sv.y >> 4) < TR_VIEW_PX(731));
	}

	/* 3. Deterministic: rebuilding, and a fresh scene fed the same input,
	 * give a bit-identical DL and camera. */
	{
		tr_scene_t s2;
		tr_cam_t   cam2;

		tr_scene_build(&s, &in, &cam, &dl);
		tr_scene_init(&s2);
		tr_scene_step(&s2, &in);
		tr_scene_build(&s2, &in, &cam2, &dl2);
		assert(dl.n == dl2.n && memcmp(dl.tri, dl2.tri, dl.n * sizeof(dl.tri[0])) == 0);
		assert(memcmp(&cam, &cam2, sizeof(cam)) == 0);
	}

	/* 3b. The two-core split: part 1's DL then part 2's == the whole DL. */
	{
		static tr_dl_t p1, p2;
		tr_cam_t       c1, c2;

		tr_scene_build(&s, &in, &cam, &dl);
		tr_scene_build_part(&s, &in, &c1, &p1, 1);
		tr_scene_build_part(&s, &in, &c2, &p2, 2);
		assert(p1.n > 0 && p2.n > 0 && p1.n + p2.n == dl.n);
		assert(memcmp(p1.tri, dl.tri, p1.n * sizeof(dl.tri[0])) == 0);
		assert(memcmp(p2.tri, &dl.tri[p1.n], p2.n * sizeof(dl.tri[0])) == 0);
		assert(memcmp(&c1, &cam, sizeof(cam)) == 0 && memcmp(&c2, &cam, sizeof(cam)) == 0);
		printf("split: part 1 %u + part 2 %u tris\n", p1.n, p2.n);
	}

	/* 3b2. The ground is a NOZ background, so it cannot hide what is under
	 * the board: a particle wholly below y = 0 emits nothing, one crossing
	 * it still does (cut at the board). */
	{
		tr_scene_t q = s;
		uint16_t   n0;

		memset(q.p, 0, sizeof(q.p));
		tr_scene_build(&q, &in, &cam, &dl);
		n0        = dl.n;
		q.p[0]    = (tr_particle_t){{0.0f, -40.0f, 600.0f}, {0.0f, 0.0f, 0.0f}, 5.0f, 5, 0};
		tr_scene_build(&q, &in, &cam, &dl);
		assert(dl.n == n0);
		q.p[0].pos.y = 2.0f;
		tr_scene_build(&q, &in, &cam, &dl);
		assert(dl.n > n0);
	}

	/* 3c. LOD knob: every TR_LOD_* bit only removes work. */
	{
		tr_scene_t q = s;

		tr_scene_build(&s, &in, &cam, &dl);
		q.quality = TR_LOD_NO_BACK_RANK | TR_LOD_NEAR;
		tr_scene_build(&q, &in, &cam, &dl2);
		printf("lod: %u -> %u tris\n", dl.n, dl2.n);
		assert(dl2.n < dl.n && tr_dl_dropped == 0);
	}

	/* 3d. Shoulder parts (P4): every wall mesh stands on the board (y >= 0,
	 * the NOZ ground hides nothing below it), every far LOD is cheaper
	 * than its full mesh, and over a whole stretch of scrolling -- every
	 * hash choice of part at every depth -- no DL vertex goes under the
	 * board and every band's bin fits. */
	{
		tr_frame_in_t none = tr_scene_golden_in(0, 1);
		float         lowest = 1e30f;
		uint32_t      worst = 0, most = 0;

		for (int k = 0; k < TR_WALL_LOD_N; k++) {
			for (int j = 0; j < 2; j++) {
				const tr_mesh_t *m = tr_wall_lod[k][j];

				for (int i = 0; m && i < m->nv; i++) {
					assert(m->v[i * 3 + 1] >= 0);
				}
			}
			assert(!tr_wall_lod[k][1] || tr_wall_lod[k][1]->nt < tr_wall_lod[k][0]->nt);
		}
		memset(none.ents, 0, sizeof(none.ents));
		tr_scene_init(&s);
		for (uint32_t t = 0; t < 400; t += 3) {
			uint32_t b;

			none.tick = t;
			tr_scene_step(&s, &none);
			tr_scene_build(&s, &none, &cam, &dl);
			float y = min_world_y(&dl, &cam);

			lowest = y < lowest ? y : lowest;
			b      = render(&cam, &none);
			worst  = b > worst ? b : worst;
			most   = dl.n > most ? dl.n : most;
		}
		printf("walls: %d part meshes on the board, lowest vertex y %.2f, over 134 scroll steps max %u tris, "
		       "max band bin %u\n", TR_WALL_LOD_N, (double)lowest, (unsigned)most, (unsigned)worst);
		assert(lowest > -1.5f && worst < TR_BIN_MAX && tr_dl_dropped == 0);
	}

	/* 3e. The shoulder cull (wall(): parts whose sphere projects off
	 * screen are skipped). TR_WALL_R bounds every wall mesh at any yaw and
	 * at TR_WALL_SY_MAX with 10 % to spare; and over play frames with full
	 * lane-change banks and shaking crash frames the culled image equals
	 * the unculled one pixel for pixel. The bound is what catches a radius
	 * cut too far: the pixel check alone still passed at 200 (the review's
	 * 2,080 frames; 100 failed there). */
	{
		float far = 0.0f;

		for (int k = 0; k < TR_WALL_LOD_N; k++) {
			for (int j = 0; j < 2; j++) {
				const tr_mesh_t *m = tr_wall_lod[k][j];

				for (int i = 0; m && i < m->nv; i++) {
					float x = (float)m->v[i * 3], y = (float)m->v[i * 3 + 1] * TR_WALL_SY_MAX - 100.0f;
					float z = (float)m->v[i * 3 + 2], d = x * x + y * y + z * z;

					far = d > far ? d : far;
				}
			}
		}
		printf("walls: farthest vertex %.1f from the cull centre (TR_WALL_R %.0f)\n", sqrt((double)far),
		       (double)TR_WALL_R);
		assert(far * 1.21f <= TR_WALL_R * TR_WALL_R);

		static uint16_t ref[W * H];
		int             frames = 0;

		for (int f = 0; f < 60; f++) {
			tr_frame_in_t a = tr_scene_golden_in(500 + 7 * (uint32_t)f, (uint8_t)((f / 3) % 3));

			if (f >= 40) {
				a = crash_in((uint8_t)(f - 40), f & 3);
			}
			tr_scene_init(&s);
			tr_scene_step(&s, &a);
			a.lane = (uint8_t)((a.lane + 1 + (f & 1)) % 3); /* mid lane change: full bank */
			tr_scene_step(&s, &a);
			tr_scene_wall_r = 1e30f;
			tr_scene_build(&s, &a, &cam, &dl);
			render(&cam, &a);
			memcpy(ref, fb, sizeof(fb));
			tr_scene_wall_r = TR_WALL_R;
			tr_scene_build(&s, &a, &cam, &dl);
			render(&cam, &a);
			assert(memcmp(ref, fb, sizeof(fb)) == 0);
			frames++;
		}
		printf("walls: culled == unculled over %d frames\n", frames);
	}

	/* 3f. The living board (P12). */
	{
		/* a. Determinism: a frame is a function of tick + phase (a fresh
		 * scene rebuilds the same DL); the next phase changes it. */
		{
			static tr_dl_t d0, d1;
			tr_frame_in_t  a = tr_scene_golden_in(777, 2), b = a;
			tr_scene_t     q;

			a.flags |= TR_FLAG_PHASE, a.phase = 0x4000;
			b = a, b.phase = 0x8000;
			tr_scene_init(&q);
			tr_scene_step(&q, &a);
			tr_scene_build(&q, &a, &cam, &d0);
			tr_scene_init(&q);
			tr_scene_step(&q, &a);
			tr_scene_build(&q, &a, &cam, &d1);
			assert(d0.n == d1.n && memcmp(d0.tri, d1.tri, d0.n * sizeof(d0.tri[0])) == 0);
			tr_scene_step(&q, &b);
			tr_scene_build(&q, &b, &cam, &d1);
			assert(d0.n != d1.n || memcmp(d0.tri, d1.tri, d0.n * sizeof(d0.tri[0])) != 0);
		}

		/* b. Fans and LEDs are placed by the tile hash: a fan only on a QFP
		 * (x 510, top 30) or a BGA (x 500, top 44) -- the part walls() draws
		 * from the same hash (d.h) -- nearer than LOD_Z, on
		 * about 3 in 4 of them (2 in 8 tile sides carry one); LEDs on the
		 * board at the lane edge of the shoulder, clear of the lane (360)
		 * and of the back rank (760); the same tile gives the same deco;
		 * a blinking LED is lit about half the time, a chasing row one LED
		 * at a time. */
		{
			tr_frame_in_t a    = tr_scene_golden_in(0, 1);
			int           fans = 0, rows1 = 0, rows3 = 0, sides = 0;

			for (uint32_t tile = 0; tile < 4000; tile++) {
				for (int side = -1; side <= 1; side += 2) {
					tr_deco_t d, e, f;

					tr_scene_deco(tile, side, 600.0f, 0, &a, &d);
					tr_scene_deco(tile, side, 600.0f, 0, &a, &e);
					assert(memcmp(&d, &e, sizeof(d)) == 0);
					tr_scene_deco(tile, side, 2000.0f, 0, &a, &f);
					assert(f.fan.y == 0.0f); /* LOD: no fans far off */
					/* walls()' threshold, TR_LOD_NEAR included: at 1200 a fan
					 * only at full detail, and the LEDs go low-poly with the parts */
					tr_scene_deco(tile, side, 1200.0f, 0, &a, &e);
					tr_scene_deco(tile, side, 1200.0f, TR_LOD_NEAR, &a, &f);
					assert(f.fan.y == 0.0f && e.fan.y == d.fan.y && f.n_led == e.n_led);
					assert(!e.led_lo && (!f.n_led || f.led_lo) && !d.led_lo);
					/* animated by the sub-tick phase too, not whole ticks only
					 * (0.5x pace: a tick every other frame) */
					if (d.fan.y != 0.0f) {
						tr_frame_in_t h = a;

						h.flags |= TR_FLAG_PHASE, h.phase = 0x8000;
						tr_scene_deco(tile, side, 600.0f, 0, &h, &e);
						assert(e.fan_yaw != d.fan_yaw);
					}
					sides++;
					if (d.fan.y != 0.0f) {
						fans++;
						assert(((d.h & 7u) == 3u && d.fan.y == 30.0f && d.fan.x == side * 510.0f) ||
						       ((d.h & 7u) == 4u && d.fan.y == 44.0f && d.fan.x == side * 500.0f));
						assert(d.fan.z == 600.0f + TR_TILE_LEN * 0.5f);
					}
					assert(d.n_led == 0 || d.n_led == 1 || d.n_led == 3);
					if (d.n_led) {
						float xi = d.led.x * (float)side, xo = xi + 2.0f * 34.0f;

						assert(d.led_col < 4 && d.led.y == 0.0f && xi - 10.0f > 360.0f && xo + 10.0f < 760.0f);
						assert(d.led.z > 600.0f && d.led.z < 650.0f); /* the gap before the tile's parts */
						rows1 += d.n_led == 1, rows3 += d.n_led == 3;
					}
				}
			}
			printf("living: %d tile sides, %d fans, %d single LEDs, %d LED rows\n", sides, fans, rows1, rows3);
			assert(fans > sides * 14 / 100 && fans < sides * 24 / 100);
			assert(rows1 > sides * 4 / 10 && rows3 > sides * 2 / 10);

			/* over 64 ticks in 1/16 steps: blink ~half on, chase <= 1 lit and
			 * every LED of the row gets its turn; a fan's rotor turns. */
			for (uint32_t tile = 0; tile < 200; tile++) {
				tr_deco_t d;
				int       on = 0, all = 0, lit[3] = {0};
				float     yaw0;

				tr_scene_deco(tile, 1, 600.0f, 0, &a, &d);
				yaw0 = d.fan_yaw;
				if (!d.n_led) {
					continue;
				}
				for (uint32_t q = 0; q < 64 * 16; q++) {
					tr_frame_in_t b = a;

					b.tick = 1000 + q / 16, b.flags |= TR_FLAG_PHASE, b.phase = (uint16_t)((q & 15u) << 12);
					tr_scene_deco(tile, 1, 600.0f, 0, &b, &d);
					all++;
					if (d.n_led == 1) {
						assert(d.led_on <= 1);
						on += d.led_on;
					} else {
						assert(d.led_on == 0 || d.led_on == 1 || d.led_on == 2 || d.led_on == 4);
						for (int k = 0; k < 3; k++) {
							lit[k] += (d.led_on >> k) & 1;
						}
					}
				}
				if (d.n_led == 1) {
					assert(on > all * 4 / 10 && on < all * 6 / 10);
				} else {
					assert(lit[0] > all / 6 && lit[1] > all / 6 && lit[2] > all / 6);
				}
				if (d.fan.y != 0.0f) {
					assert(d.fan_yaw != yaw0);
				}
			}
			for (int m = 0; m < 2; m++) { /* the fan stands on its package */
				const tr_mesh_t *fm = m ? &tr_mesh_fan : &tr_mesh_fan_frame;

				for (int i = 0; i < fm->nv; i++) {
					assert(fm->v[i * 3 + 1] >= 0 && fm->v[i * 3] * fm->v[i * 3] + fm->v[i * 3 + 2] * fm->v[i * 3 + 2] <= 2 * 54 * 54);
				}
			}
		}

		/* c. Budget proxy (host): over a stretch of play frames, what the
		 * living board adds to the DL -- triangles, screen rows and pixels
		 * (union) -- against the same frame with TR_LOD_STILL. Silicon unit
		 * costs (docs/2026-09-22-measurements.md): tri setup ~1.65 us, a row
		 * ~134 cyc, a z-tested Gouraud px ~25 cyc at 800 MHz, split over two
		 * cores. */
		{
			static tr_dl_t d1;
			uint32_t       wt = 0, wr = 0, wp = 0, st = 0, sr = 0, sp = 0, nf = 0;
			tr_scene_t     q;

			tr_scene_init(&q);
			for (uint32_t t = 0; t < 600; t += 5) {
				tr_frame_in_t a = tr_scene_golden_in(t, (uint8_t)((t / 50) % 3));
				uint32_t      rows, px, dt;

				a.flags |= TR_FLAG_PHASE, a.phase = (uint16_t)(t * 0x2F1u);
				tr_scene_step(&q, &a);
				q.quality = TR_LOD_STILL;
				tr_scene_build(&q, &a, &cam, &dl);
				q.quality = 0;
				tr_scene_build(&q, &a, &cam, &d1);
				assert(d1.n > dl.n && tr_dl_dropped == 0);
				/* the extra triangles are interleaved with the parts: find them by
				 * walking both lists (the still DL is a subsequence of the full one) */
				{
					static tr_dl_t extra;
					uint16_t       j = 0;

					extra.n = 0;
					for (uint16_t i = 0; i < d1.n; i++) {
						if (j < dl.n && memcmp(&d1.tri[i], &dl.tri[j], sizeof(dl.tri[0])) == 0) {
							j++;
						} else {
							extra.tri[extra.n++] = d1.tri[i];
						}
					}
					assert(j == dl.n);
					cover(&extra, 0, extra.n, &rows, &px);
					dt = extra.n;
				}
				wt = dt > wt ? dt : wt, wr = rows > wr ? rows : wr, wp = px > wp ? px : wp;
				st += dt, sr += rows, sp += px, nf++;
			}
			double mean = ((double)st / nf * 1.65e-3 + (double)sr / nf * 134 / 800e3 + (double)sp / nf * 25 / 800e3) / 2;
			double peak = ((double)wt * 1.65e-3 + (double)wr * 134 / 800e3 + (double)wp * 25 / 800e3) / 2;

			printf("living budget: +%u tris, +%u rows, +%u px mean; worst +%u / +%u / +%u; est +%.2f ms/core mean, "
			       "+%.2f worst (raster side)\n", (unsigned)(st / nf), (unsigned)(sr / nf), (unsigned)(sp / nf), (unsigned)wt,
			       (unsigned)wr, (unsigned)wp, mean, peak);
			assert(wt <= 480 && wr <= 5200 && wp <= 48000);
		}
	}

	/* 4. Worst case (a superset of anything step.c can spawn): all 16
	 * slots live as the heaviest mesh (full-detail resistor) packed inside
	 * the full-detail range, mid-jump, mid-bank, a full particle burst --
	 * still inside TR_DL_MAX_TRIS and every band's bin. */
	for (int c = 0; c < TR_CHARS; c++) { /* P16b: every character (Solder the heaviest) */
		tr_frame_in_t w = tr_scene_golden_in(4321, 0);
		uint32_t      bin;

		for (int i = 0; i < 16; i++) {
			w.ents[i] = (tr_pkt_ent_t){1, (uint8_t)(i % 3), 1, 0, (int16_t)(900 + 16 * i), 0};
		}
		w.flags |= TR_FLAG_CHAR;
		w.character = (uint8_t)c;
		tr_scene_init(&s);
		tr_scene_step(&s, &w);
		w.lane = 2;
		w.score += 10;
		w.flags |= TR_FLAG_AIRBORNE;
		w.air_ticks = TR_AIR_TICKS / 2;
		tr_scene_step(&s, &w);
		tr_scene_build(&s, &w, &cam, &dl);
		bin = render(&cam, &w);
		printf("worst %s: %u tris, dropped %u, max band bin %u of %u\n", tr_rig_chars[c].name, dl.n,
		       (unsigned)tr_dl_dropped, (unsigned)bin, TR_BIN_MAX);
		assert(dl.n < TR_DL_MAX_TRIS && tr_dl_dropped == 0 && bin < TR_BIN_MAX);
	}

	/* 5. Golden frame, band by band like the A32; timing informational. */
	{
		int64_t t0, t1, t2;

		tr_scene_init(&s);
		tr_scene_step(&s, &in);
		t0 = now_ns();
		for (int i = 0; i < 100; i++) {
			tr_scene_build(&s, &in, &cam, &dl);
		}
		t1 = now_ns();
		for (int i = 0; i < 20; i++) {
			render(&cam, &in);
		}
		t2 = now_ns();

		uint32_t crc = crc32(fb, sizeof(fb));

		printf("golden scene crc32 %08x (%u tris); host ns/frame: build %lld, bin+raster %lld\n", (unsigned)crc,
		       dl.n, (long long)((t1 - t0) / 100), (long long)((t2 - t1) / 20));
		if (getenv("TR_DUMP")) {
			dump("scene");
		}
		assert(crc == TR_SCENE_GOLDEN_CRC);
	}

	/* 6. Crash (P6). */
	{
		static tr_dl_t d0, d1;
		tr_scene_t     s2;
		tr_cam_t       cam2;

		/* a. The hit obstacle is drawn touching the runner's front, not
		 * inside it (collision was judged at the runner line), and only
		 * the named slot moves. */
		for (int low = 0; low <= 1; low++) {
			tr_frame_in_t c    = crash_in(0, low);
			float         ob   = mesh_zmax(low ? &tr_mesh_resistor : &tr_mesh_arch);
			float         run  = (float)TR_PROJ_Z_RUNNER + TR_RUNNER_FRONT_Z; /* case 0 + 6f */
			float         zhit = tr_scene_ent_z(&c, 4);

			assert(zhit - ob >= run);
			assert(tr_scene_ent_z(&c, 9) == (float)tr_proj_depth_of_model_y(c.ents[9].y));
			c.flags = TR_FLAG_ALIVE; /* not a crash frame, past the line: no clamp */
			assert(tr_scene_ent_z(&c, 4) == (float)tr_proj_depth_of_model_y(1110));
		}

		/* a2. Before the fatal tick: an obstacle in the runner's lane, not
		 * yet at its line, that the current pose does not clear is already
		 * held at the contact depth -- the fatal frame must not jump it
		 * backwards (play, and attract's sub-tick extrapolation). A pose
		 * that clears it, another lane, or one already past the line (it
		 * was cleared) is left alone. */
		for (int low = 0; low <= 1; low++) {
			tr_frame_in_t pre = tr_scene_golden_in(1234, 1), fatal = crash_in(0, low);
			float         zc  = tr_scene_ent_z(&fatal, 4);

			pre.ents[4] = (tr_pkt_ent_t){1, 1, (uint8_t)low, 0, 1100, 0};
			assert(tr_scene_ent_z(&pre, 4) >= zc);
			pre.ents[4].y = 1090;
			pre.phase     = 65000;
			pre.flags |= TR_FLAG_PHASE;
			assert(tr_scene_ent_z(&pre, 4) >= zc);
			pre.flags |= low ? TR_FLAG_AIRBORNE : TR_FLAG_DUCKING;
			assert(tr_scene_ent_z(&pre, 4) < zc);
			pre.flags     = TR_FLAG_ALIVE;
			pre.lane      = 0;
			pre.ents[4].y = 1100;
			assert(tr_scene_ent_z(&pre, 4) < zc);
			pre.lane      = 1;
			pre.phase     = 0;
			pre.ents[4].y = 1110;
			assert(tr_scene_ent_z(&pre, 4) == (float)tr_proj_depth_of_model_y(1110));
		}
		/* The same for a live wire (P4b, kind 3) of either height: held at
		 * the runner's front before the fatal tick, where the fatal wire
		 * crash frame draws it. */
		for (int low = 0; low <= 1; low++) {
			tr_frame_in_t pre = tr_scene_golden_in(1234, 1), fatal = crash_in(0, 2 + low);
			float         zc  = tr_scene_ent_z(&fatal, 4);

			assert(zc == tr_scene_ent_z(&fatal, 4) && zc >= (float)TR_PROJ_Z_RUNNER + TR_RUNNER_FRONT_Z + 24.0f);
			pre.ents[4] = (tr_pkt_ent_t){3, 1, (uint8_t)low, 0, 1100, 0};
			assert(tr_scene_ent_z(&pre, 4) == zc);
			pre.flags |= low ? TR_FLAG_AIRBORNE : TR_FLAG_DUCKING;
			assert(tr_scene_ent_z(&pre, 4) < zc);
		}

		/* b. The first crash frame bursts every particle, as small sparks
		 * (r <= 8.5) that never come nearer than the runner; the burst
		 * moves at SLOWMO in the first frames and at full speed later. */
		for (uint8_t ct = 0; ct < TR_CRASH_TICKS; ct++) {
			crash_run(&s, ct, 1, &cam, &dl);
			for (int i = 0; i < TR_PARTICLES; i++) {
				float r = tr_scene_particle_r(&s.p[i]);

				assert(s.p[i].spark && r <= 8.5f);
				assert(r == 0.0f || s.p[i].pos.z >= (float)TR_PROJ_Z_RUNNER - 40.0f);
			}
		}
		crash_run(&s, 0, 1, &cam, &dl);
		assert(live_particles(&s) == TR_PARTICLES);
		{
			tr_v3_t       p0 = s.p[0].pos, v0 = s.p[0].vel;
			tr_frame_in_t c1 = crash_in(1, 1);

			tr_scene_step(&s, &c1);
			float         d  = (s.p[0].pos.x - p0.x) - v0.x * 0.35f;

			assert(v0.x != 0.0f && d < 1e-3f && d > -1e-3f);
		}

		/* c. Deterministic: two fresh scenes, same crash, same DL; the
		 * animation moves on between crash ticks. */
		crash_run(&s, 20, 1, &cam, &d0);
		crash_run(&s2, 20, 1, &cam2, &d1);
		assert(d0.n == d1.n && memcmp(d0.tri, d1.tri, d0.n * sizeof(d0.tri[0])) == 0);
		assert(memcmp(&cam, &cam2, sizeof(cam)) == 0);
		crash_run(&s2, 30, 1, &cam2, &d1);
		assert(d0.n != d1.n || memcmp(d0.tri, d1.tri, d0.n * sizeof(d0.tri[0])) != 0);

		/* d. Red flash: border tris + a redder background at the hit,
		 * gone by the end; inside the budget throughout. */
		for (uint8_t ct = 0; ct < TR_CRASH_TICKS; ct++) {
			tr_frame_in_t c = crash_in(ct, ct & 1);
			tr_bg_t       plain, fl;

			crash_run(&s, ct, ct & 1, &cam, &dl);
			assert(dl.n < TR_DL_MAX_TRIS && tr_dl_dropped == 0);
			tr_scene_bg(&c, &cam, &plain);
			fl = plain;
			tr_scene_bg_flash(&c, &fl);
			if (ct == 0) {
				assert(dl.tri[dl.n - 1].a[0].w == 0xFFFF && (dl.tri[dl.n - 1].c >> 11) > 25);
				assert((fl.top >> 11) > (plain.top >> 11) && (fl.ground >> 11) > (plain.ground >> 11));
				/* the halo turns red and the stars all but go out */
				assert((fl.halo & 0x1F) < (plain.halo & 0x1F) && plain.star_dim == 0 && fl.star_dim > 200);
				render(&cam, &c); /* asserts every band's bin fits */
			}
			if (ct == TR_CRASH_TICKS - 1) {
				assert(memcmp(&fl, &plain, sizeof(fl)) == 0);
				assert(dl.tri[dl.n - 1].a[0].w != 0xFFFF);
			}
		}

		/* e. Nothing under the board: the ground is a NOZ background
		 * (hides nothing), so no crash frame -- tumbling runner, knocked
		 * obstacle, shards -- and not the duck pose may emit a vertex
		 * below y = 0. */
		float lowest = 1e30f;

		for (uint8_t ct = 0; ct < TR_CRASH_TICKS; ct++) {
			for (int low = 0; low < 4; low++) { /* 2, 3: live wires */
				float y;

				crash_run(&s, ct, low, &cam, &dl);
				y      = min_world_y(&dl, &cam);
				lowest = y < lowest ? y : lowest;
			}
		}
		{
			tr_frame_in_t dk = tr_scene_golden_in(1234, 1);
			float         y;

			dk.flags |= TR_FLAG_DUCKING;
			tr_scene_init(&s);
			for (int k = 0; k < 6; k++, dk.tick++) { /* the duck blend eases in over 5 frames */
				tr_scene_step(&s, &dk);
			}
			tr_scene_build(&s, &dk, &cam, &dl);
			y      = min_world_y(&dl, &cam);
			lowest = y < lowest ? y : lowest;
			tr_scene_runner(&s, &dk, &rd);
			assert(runner_top(&rd) < 145.0f); /* the duck pose is drawn: under the arch bar */
		}
		printf("crash + duck: lowest vertex y %.2f\n", (double)lowest);
		assert(lowest > -1.5f);

		/* f. The runner as drawn. A crash frame pivots the tumble on the
		 * rearmost skinned point (local z exactly 0), so tipping back
		 * needs no lift off the board; and in every scene-time blend --
		 * run, lane change, jump, landing, duck -- its front stays inside
		 * the contact bound. */
		float lift = 0.0f, fz = -1e30f, top = -1e30f;

		for (uint8_t ct = 0; ct < TR_CRASH_TICKS; ct++) {
			tr_frame_in_t c = crash_in(ct, ct & 1);

			crash_run(&s, ct, ct & 1, &cam, &dl);
			tr_scene_runner(&s, &c, &rd);
			assert(runner_z(&rd, 0) == 0.0f);
			/* the lift the pose itself needs upright (a blend into the
			 * crash pose can reach a little under), vs the one given */
			float lo = 1e30f, need;

			for (int p = 0; p < TR_RIG_DRAWN; p++) {
				for (int i = 0; i < rd.mesh[p].nv; i++) {
					lo = rd.xyz[p][i * 3 + 1] < lo ? rd.xyz[p][i * 3 + 1] : lo;
				}
			}
			need = -(lo + rd.inst.pos.y - rd.board_lift);
			need = need > 0.0f ? need : 0.0f;
			lift = rd.board_lift - need > lift ? rd.board_lift - need : lift;
		}
		float end_cos = 0.0f, roll_max = 0.0f, pullback = 0.0f;

		{
			/* As step.c publishes them: airborne TR_AIR_TICKS - 1 ticks
			 * (air_ticks 13..1), ducking TR_DUCK_TICKS - 1 (duck_ticks
			 * 11..1). A jump, a duck, a lane change, then a duck with a
			 * lane change in the middle of it. */
			tr_frame_in_t a = tr_scene_golden_in(1000, 1);

			tr_scene_init(&s);
			for (int fr = 0; fr < 140; fr++, a.tick++) {
				int j = fr - 10, d = fr < 90 ? fr - 40 : fr - 90;
				int air = j >= 0 && j < TR_AIR_TICKS - 1, duck = d >= 0 && d < TR_DUCK_TICKS - 1;

				a.lane       = (uint8_t)(fr < 60 ? 1 : fr < 92 ? 0 : fr < 115 ? 2 : 1);
				a.flags      = TR_FLAG_ALIVE | (air ? TR_FLAG_AIRBORNE : 0u) | (duck ? TR_FLAG_DUCKING : 0u);
				a.air_ticks  = (uint8_t)(air ? TR_AIR_TICKS - 1 - j : 0);
				a.duck_ticks = (uint8_t)(duck ? TR_DUCK_TICKS - 1 - d : 0);
				tr_scene_step(&s, &a);
				tr_scene_runner(&s, &a, &rd);
				{
					float t, f;

					runner_world(&rd, &t, &f);
					fz = f > fz ? f : fz;
					/* worst_front-style bounds (fz above) can't fail: the
					 * clamp always leaves the measured front inside bound
					 * by construction. hold_front()'s own pull-back can't
					 * hide that way -- over this whole run + lane-change +
					 * duck-roll sequence it should stay ~0 (roll_about_middle()
					 * already holds the roll's front correctly); a
					 * regression there (it once reached 2.45 u on the
					 * duck roll here, before roll_about_middle() counted
					 * the instance yaw) still shows up. */
					pullback = tr_front_pullback > pullback ? tr_front_pullback : pullback;
					if (duck && d >= 2) {
						top = t > top ? t : top; /* curled (2 frames) and rolling: under the arch bar */
					}
					if (duck) {
						roll_max = rd.inst.pitch > roll_max ? rd.inst.pitch : roll_max;
						end_cos  = cosf(rd.inst.pitch); /* the last duck frame's */
					}
				}
			}
		}
		printf("runner: crash tumble extra lift %.2f, front z over blends %.1f, top while ducking %.1f, hold_front "
		       "pulled back %.3f u worst\n",
		       (double)lift, (double)fz, (double)top, (double)pullback);
		assert(lift < 0.5f && fz <= TR_RUNNER_FRONT_Z && top < 145.0f);
		assert(pullback <= 0.05f);
		/* the duck really rolls: a whole turn, upright again at its end */
		printf("runner: duck roll to %.2f rad, cos at the end %.3f\n", (double)roll_max, (double)end_cos);
		assert(roll_max > 6.0f && end_cos > 0.99f);

		/* g. Lane change: a side hop of TR_LANE_HOP over the run it would
		 * have been, and none out of a duck. */
		{
			tr_scene_t    s1, s2;
			tr_frame_in_t a = tr_scene_golden_in(1000, 1), b;
			float         hop = 0.0f;

			tr_scene_init(&s1);
			tr_scene_init(&s2);
			for (int fr = 0; fr < 30; fr++, a.tick++) {
				b      = a;
				b.lane = (uint8_t)(fr < 10 ? 1 : 2);
				tr_scene_step(&s1, &a);
				tr_scene_step(&s2, &b);
				hop = s2.ch[TR_ANIM_ROOT_Y] - s1.ch[TR_ANIM_ROOT_Y] > hop ? s2.ch[TR_ANIM_ROOT_Y] - s1.ch[TR_ANIM_ROOT_Y]
											  : hop;
			}
			printf("runner: lane-change hop %.1f (TR_LANE_HOP %.0f)\n", (double)hop, (double)TR_LANE_HOP);
			assert(hop > TR_LANE_HOP - 3.0f && hop < TR_LANE_HOP + 1.0f);

			/* ... and none while ducking: the same duck with and without a
			 * lane change keeps the same hip height */
			a = tr_scene_golden_in(1000, 1);
			tr_scene_init(&s1);
			tr_scene_init(&s2);
			for (int fr = 0; fr < 14; fr++, a.tick++) {
				a.flags      = TR_FLAG_ALIVE | (fr < 11 ? TR_FLAG_DUCKING : 0u);
				a.duck_ticks = (uint8_t)(fr < 11 ? TR_DUCK_TICKS - 1 - fr : 0);
				b            = a;
				b.lane       = (uint8_t)(fr < 2 ? 1 : 2);
				tr_scene_step(&s1, &a);
				tr_scene_step(&s2, &b);
				if (fr < 11) {
					assert(s1.ch[TR_ANIM_ROOT_Y] == s2.ch[TR_ANIM_ROOT_Y]);
				}
			}
		}

		/* h. Hit mid-roll (ducked into a low part): the roll angle eases
		 * into the tumble, no pop upright -- from the last rolling frame
		 * on, the drawn pitch moves less than 0.9 rad a frame (the roll
		 * itself turns ~0.95 a frame; popping upright would be ~2.4). */
		{
			tr_frame_in_t a = tr_scene_golden_in(1000, 1);
			float         prev = 0.0f, jump = 0.0f, rolled = 0.0f;

			tr_scene_init(&s);
			for (int fr = 0; fr < 30; fr++, a.tick++) {
				int d = fr - 5, hit = fr >= 12;

				a.flags      = hit ? TR_FLAG_CRASH : TR_FLAG_ALIVE | (d >= 0 ? TR_FLAG_DUCKING : 0u);
				a.duck_ticks = (uint8_t)(!hit && d >= 0 ? TR_DUCK_TICKS - 1 - d : 0);
				a.crash_tick = (uint8_t)(hit ? fr - 12 : 0);
				a.crash_lane = 1, a.crash_kind = TR_CRASH_KIND_LOW, a.crash_ent = 0;
				tr_scene_step(&s, &a);
				tr_scene_runner(&s, &a, &rd);
				if (fr == 11) {
					rolled = rd.inst.pitch;
				}
				if (fr >= 12) { /* from the last rolling frame into the crash */
					float dp = rd.inst.pitch - prev;

					dp   = dp < 0.0f ? -dp : dp;
					jump = dp > jump ? dp : jump;
				}
				prev = rd.inst.pitch;
				{
					float t, f;

					runner_world(&rd, &t, &f);
					assert(f <= TR_RUNNER_FRONT_Z + 40.0f); /* knocked back from the contact */
				}
			}
			printf("runner: crash mid-roll (rolled %.2f rad): worst pitch step %.2f rad a frame\n", (double)rolled,
			       (double)jump);
			assert(rolled > 1.0f && jump < 0.9f);
		}

		printf("crash: contact ok (and before the hit), burst %d, slow-mo ok, deterministic, flash fades\n", TR_PARTICLES);
	}

	/* 7. Attract interpolation (P7): the sub-tick phase moves entities and
	 * the scroll smoothly between whole ticks. */
	{
		static tr_dl_t d0, d1, dh;
		tr_frame_in_t  a = tr_scene_golden_in(1234, 1), b = a, h = a;

		b.tick++;
		h.phase = 32768;
		h.flags |= TR_FLAG_PHASE;
		for (int i = 0; i < 16; i++) {
			float z0 = tr_scene_ent_z(&a, i), zh = tr_scene_ent_z(&h, i);
			float z1 = (float)tr_proj_depth_of_model_y((int16_t)(a.ents[i].y + TR_SCROLL_PX));

			assert(zh < z0 && zh > z1 && zh - (z0 + z1) * 0.5f < 1.0f && (z0 + z1) * 0.5f - zh < 1.0f);
		}
		tr_scene_init(&s);
		tr_scene_step(&s, &a);
		tr_scene_build(&s, &a, &cam, &d0);
		tr_scene_build(&s, &h, &cam, &dh);
		tr_scene_build(&s, &b, &cam, &d1);
		assert(d0.n != dh.n || memcmp(d0.tri, dh.tri, d0.n * sizeof(d0.tri[0])) != 0);
		assert(d1.n != dh.n || memcmp(d1.tri, dh.tri, d1.n * sizeof(d1.tri[0])) != 0);
		/* The ground scrolls with the phase too (tiles and entities
		 * agree): half a tick lands half way between the whole ticks. */
		{
			uint32_t s0 = tr_scene_scroll(1234, 0), s1 = tr_scene_scroll(1235, 0);
			uint32_t sh = tr_scene_scroll(1234, 32768);

			assert(s0 < sh && sh < s1 && 2u * sh + 2u >= s0 + s1 && 2u * sh <= s0 + s1 + 2u);
			assert(tr_scene_scroll(1234, 65535) <= s1 && tr_scene_scroll(1234, 65535) + 1u >= s1);
		}

		/* The phase counts only under TR_FLAG_PHASE: an old HE leaves
		 * those bytes unwritten. */
		{
			tr_frame_in_t u = h;

			u.flags &= ~TR_FLAG_PHASE;
			tr_scene_build(&s, &u, &cam, &dh);
			assert(dh.n == d0.n && memcmp(dh.tri, d0.tri, d0.n * sizeof(d0.tri[0])) == 0);
			assert(tr_scene_ent_z(&u, 3) == tr_scene_ent_z(&a, 3));
		}
		printf("attract: half-tick phase lands between the ticks\n");

		/* 0.5x play (state.h TR_GAME_PACE_Q8): a step every other frame,
		 * the frames between at phase 1/2 -- every entity comes nearer on
		 * EVERY frame, the scroll never goes back, the run cycle moves on
		 * a little each frame, particles at half speed. */
		{
			tr_scene_t    sp;
			tr_frame_in_t f = a;
			float         prev_z[16], prev_root = 0.0f, worst_root = 0.0f;
			uint32_t      prev_s = 0;
			float         prev_cyc = -1.0f;

			tr_scene_init(&sp);
			for (int fr = 0; fr < 40; fr++) {
				f.tick    = a.tick + (uint32_t)(fr / 2);
				f.phase   = (uint16_t)((fr & 1) ? 32768u : 0u);
				f.pace_q8 = TR_GAME_PACE_Q8;
				f.flags   = (a.flags & ~TR_FLAG_PHASE) | ((fr & 1) ? TR_FLAG_PHASE : 0u);
				for (int i = 0; i < 16; i++) { /* the game moves them a step at a time */
					f.ents[i].y = (int16_t)(a.ents[i].y + (fr / 2) * TR_SCROLL_PX);
				}
				tr_scene_step(&sp, &f);

				uint32_t sc  = tr_scene_scroll(f.tick, (fr & 1) ? 32768u : 0u);
				float    cyc = ((float)(f.tick % TR_RUN_CYCLE_TICKS) + ((fr & 1) ? 0.5f : 0.0f)) / (float)TR_RUN_CYCLE_TICKS;

				for (int i = 0; i < 16; i++) {
					float z = tr_scene_ent_z(&f, i);

					if (fr > 0 && f.ents[i].kind != 0 && f.ents[i].y < tr_runner_ground_y(TR_R3D_H) - 2 * TR_SCROLL_PX) {
						assert(z < prev_z[i]); /* (nearer ones may be held at the contact depth) */
					}
					prev_z[i] = z;
				}
				{ /* the scene's own pose is the run cycle at tick + phase */
					float run[TR_ANIM_CH];

					tr_rig_run(cyc, run, 1.0f);
					assert(memcmp(run, sp.ch, sizeof(run)) == 0);
				}
				if (fr > 0) {
					float dc = cyc - prev_cyc;

					assert(sc > prev_s);
					float step = 0.5f / (float)TR_RUN_CYCLE_TICKS; /* half a tick of a stride a frame */

					assert((dc > 0.8f * step && dc < 1.2f * step) || (dc < -0.9f));
					float dr = sp.ch[TR_ANIM_ROOT_Y] - prev_root;

					dr         = dr < 0.0f ? -dr : dr;
					worst_root = dr > worst_root ? dr : worst_root;
				}
				prev_s = sc, prev_cyc = cyc, prev_root = sp.ch[TR_ANIM_ROOT_Y];
			}
			printf("pace 0.5: entities nearer every frame, hip moves <= %.2f a frame\n", (double)worst_root);
			assert(worst_root < 6.5f); /* P3d: the flight hop, 9 units in ~3 frames (a stride is 12 here; 6.04) */

			/* particles move at the game pace: a pickup burst, then one
			 * frame at 0.5x moves each by half its velocity */
			f.score += 10u;
			tr_scene_step(&sp, &f);
			{
				tr_v3_t p0 = sp.p[0].pos, v0 = sp.p[0].vel;
				float   d;

				f.tick++;
				tr_scene_step(&sp, &f);
				d = (sp.p[0].pos.x - p0.x) - 0.5f * v0.x;
				assert(v0.x != 0.0f && d < 1e-3f && d > -1e-3f);
			}
		}
	}

	/* 7b. Frame-rate independence (P3d): the scripted run (hz_in()) at 30
	 * and at 40 Hz. Compared at every tenth of a second -- a frame at both
	 * rates -- the lane slide, camera, bank, jump lift, landing dip, pose
	 * blends and pose, squash, and every particle (pickup burst, then crash
	 * sparks in slow motion) are where the other rate has them: the easing is
	 * integrated over each frame's real time, not counted in frames (a
	 * change in the packet taken as one 40 Hz frame old at both rates,
	 * r3d_scene.c ease_k). The pickup particles differ by the pace byte's
	 * rounding (170 / 256 a 30 Hz frame for 0.6667: 0.4 %); the rest to
	 * float noise. Before P3d the 30 Hz run lagged: the lane slide was 20.9
	 * units behind 0.1 s into it. And the 30 Hz run is deterministic: a
	 * fresh scene fed it again ends bit-identical. */
	{
		static tr_scene_t s30, s40, again;
		float              worst_x = 0.0f, worst_w = 0.0f, worst_ch = 0.0f, worst_p = 0.0f, worst_spark = 0.0f;
		int                compared = 0, parts = 0, air_frames = 0;

		tr_scene_init(&s30);
		tr_scene_init(&s40);
		tr_scene_init(&again);
		for (int m = 0; m <= 28; m++) { /* 0 .. 2.8 s */
			for (int f = m ? 4 * m - 3 : 0; f <= 4 * m; f++) {
				tr_frame_in_t in = hz_in((double)f / 40.0, 40);

				tr_scene_step(&s40, &in);
				if (f == 45) { /* landed at 1.1 s (frame 44): one frame on */
					assert(s40.dip == -16.0f);
				}
				if (s40.w_jump == 1.0f) { /* mid-air: no foot lock, the legs are the jump pose */
					for (int c = TR_ANIM_SIDE0; c < TR_ANIM_CH; c++) {
						int k = (c - TR_ANIM_SIDE0) % TR_ANIM_SIDE_N;

						if (k < 4) { /* thigh, knee, foot, splay */
							assert(fabsf(s40.ch[c] - s40.jump_ch[c]) < 1e-3f);
						}
					}
					air_frames++;
				}
			}
			for (int f = m ? 3 * m - 2 : 0; f <= 3 * m; f++) {
				tr_frame_in_t in = hz_in((double)f / 30.0, 30);

				tr_scene_step(&s30, &in);
				tr_scene_step(&again, &in);
				if (f == 34) { /* landed at 1.1 s (frame 33): 4/3 of a 40 Hz frame on, -8 x (3 - 4/3) */
					assert(fabsf(s30.dip + 8.0f * (3.0f - 4.0f / 3.0f)) < 1e-4f);
				}
			}
			float d[] = {s30.runner_x - s40.runner_x, s30.cam_x - s40.cam_x, (s30.cam_roll - s40.cam_roll) * 100.0f,
				     s30.lift - s40.lift, s30.dip - s40.dip, s30.cam_bob - s40.cam_bob};
			float w[] = {s30.w_jump - s40.w_jump, s30.w_duck - s40.w_duck, s30.w_crash - s40.w_crash,
				     s30.w_land - s40.w_land, s30.squash - s40.squash};

			for (unsigned k = 0; k < sizeof(d) / sizeof(d[0]); k++) {
				worst_x = fabsf(d[k]) > worst_x ? fabsf(d[k]) : worst_x;
			}
			for (unsigned k = 0; k < sizeof(w) / sizeof(w[0]); k++) {
				worst_w = fabsf(w[k]) > worst_w ? fabsf(w[k]) : worst_w;
			}
			for (int c = 0; c < TR_ANIM_CH; c++) {
				float e = fabsf(s30.ch[c] - s40.ch[c]);

				worst_ch = e > worst_ch ? e : worst_ch;
			}
			for (int i = 0; i < TR_PARTICLES; i++) {
				const tr_particle_t *a = &s30.p[i], *b = &s40.p[i];

				if (a->life > 0.5f && b->life > 0.5f) {
					float e = fabsf(a->pos.x - b->pos.x) + fabsf(a->pos.y - b->pos.y) + fabsf(a->pos.z - b->pos.z);
					float *wp = a->spark ? &worst_spark : &worst_p;

					*wp = e > *wp ? e : *wp;
					parts++;
				}
			}
			compared++;
		}
		printf("frame rate: 30 vs 40 Hz at %d common times: slides/lift/dip off by <= %.4f units, blends %.5f, "
		       "pose %.4f deg, pickup particles %.3f units, crash sparks %.4f (%d compared)\n",
		       compared, (double)worst_x, (double)worst_w, (double)worst_ch, (double)worst_p, (double)worst_spark,
		       parts);
		assert(parts > 200 && air_frames > 10);
		assert(worst_x < 0.01f && worst_w < 1e-4f && worst_ch < 0.01f && worst_spark < 0.01f && worst_p < 0.75f);
		assert(memcmp(&s30, &again, sizeof(s30)) == 0); /* deterministic */
		/* the crash time's fraction moves the knock-back: a 30 Hz frame
		 * half-way between two crash ticks is drawn half-way */
		{
			static tr_runner_draw_t r0, rh, r1;
			tr_frame_in_t           c0 = hz_in(2.1, 40), ch = c0, c1 = c0;

			ch.crash_frac = 32768u;
			c1.crash_tick++;
			tr_scene_runner(&s30, &c0, &r0);
			tr_scene_runner(&s30, &ch, &rh);
			tr_scene_runner(&s30, &c1, &r1);
			assert(rh.inst.pos.z < r0.inst.pos.z && rh.inst.pos.z > r1.inst.pos.z);
		}
		/* an old HE (hz 0) is a 40 Hz one */
		{
			static tr_scene_t o0, o40;

			tr_scene_init(&o0);
			tr_scene_init(&o40);
			for (int f = 0; f < 112; f++) {
				tr_frame_in_t in = hz_in((double)f / 40.0, 40);

				tr_scene_step(&o40, &in);
				in.hz = (uint8_t)(f & 1 ? 0 : 60); /* 0 (an old HE) and > 40 both ease as 40 */
				tr_scene_step(&o0, &in);
			}
			assert(memcmp(&o0, &o40, sizeof(o0)) == 0);
		}
	}

	/* 7c. The foot lock's weight in the scene (P3d): during a lane hop the
	 * run pose is locked at 1 - hop (the hop lifts the feet off their
	 * points), rebuilt here from its parts; and the test can tell -- on
	 * some hop frame a full-weight lock would pose the legs otherwise. */
	{
		static tr_scene_t sh;
		int               hops = 0, telling = 0;

		tr_scene_init(&sh);
		for (int f = 0; f < 20; f++) { /* the lane change lands on frame 8 */
			tr_frame_in_t in = hz_in((double)f / 40.0, 40);
			float         e[TR_ANIM_CH], e1[TR_ANIM_CH], hs, hc;
			float         cyc  = ((float)(in.tick % TR_ANIM_CYCLE) + (float)in.phase / 65536.0f) / (float)TR_ANIM_CYCLE;
			float         dx   = ((float)in.lane - 1.0f) * (float)TR_PROJ_LANE_W, left;

			tr_scene_step(&sh, &in);
			dx  -= sh.runner_x;
			left = fabsf(dx) / (float)TR_PROJ_LANE_W;
			left = left > 1.0f ? 1.0f : left;
			if (left <= 0.02f) {
				continue;
			}
			tr_sincosf(3.14159265f * (1.0f - left), &hs, &hc);
			tr_rig_run(cyc, e, 0.0f);
			e[TR_ANIM_P_ROLL] += dx * 0.08f > 14.0f ? 14.0f : (dx * 0.08f < -14.0f ? -14.0f : dx * 0.08f);
			e[TR_ANIM_ROOT_Y] += TR_LANE_HOP * hs;
			memcpy(e1, e, sizeof(e));
			tr_rig_foot_lock(cyc, e, 1.0f - hs);
			tr_rig_foot_lock(cyc, e1, 1.0f);
			for (int c = 0; c < TR_ANIM_CH; c++) {
				assert(fabsf(sh.ch[c] - e[c]) < 1e-3f);
				telling += fabsf(e1[c] - e[c]) > 0.1f;
			}
			hops++;
		}
		printf("lock weight: %d lane-hop frames posed at 1 - hop, %d channels a full lock would move\n", hops, telling);
		assert(hops >= 4 && telling > 0);
	}

	/* 8. Live wires (P4b). */
	{
		tr_v3_t a = {-100.0f, 190.0f, 900.0f}, b = {100.0f, 190.0f, 900.0f}, p0[11], p1[11], p2[11];

		/* a. The arc: ends exact, jitter bounded, one seed one polyline,
		 * another seed another. */
		tr_scene_arc(0x1234u, a, b, 20.0f, 16.0f, 10, p0);
		tr_scene_arc(0x1234u, a, b, 20.0f, 16.0f, 10, p1);
		tr_scene_arc(0x1235u, a, b, 20.0f, 16.0f, 10, p2);
		assert(memcmp(p0, p1, sizeof(p0)) == 0 && memcmp(p0, p2, sizeof(p0)) != 0);
		assert(memcmp(&p0[0], &a, sizeof(a)) == 0 && memcmp(&p0[10], &b, sizeof(b)) == 0);
		for (int k = 1; k < 10; k++) {
			float t = (float)k / 10.0f, by = 190.0f - 20.0f * 4.0f * t * (1.0f - t), dy = p0[k].y - by;

			assert(dy <= 16.01f && dy >= -16.01f && p0[k].z - 900.0f <= 4.81f && 900.0f - p0[k].z <= 4.81f);
		}

		/* b. The seed: a function of the frame (tick, phase, crash tick)
		 * and the slot, so every frame re-randomises and a rebuild does
		 * not. */
		tr_frame_in_t f = wires_in(1234), g = f;

		assert(tr_scene_wire_seed(&f, 2) == tr_scene_wire_seed(&g, 2));
		assert(tr_scene_wire_seed(&f, 2) != tr_scene_wire_seed(&f, 4));
		g.tick++;
		assert(tr_scene_wire_seed(&f, 2) != tr_scene_wire_seed(&g, 2));
		g = f, g.phase = 1000, g.flags |= TR_FLAG_PHASE;
		assert(tr_scene_wire_seed(&f, 2) != tr_scene_wire_seed(&g, 2));
		g = f, g.crash_tick = 1;
		assert(tr_scene_wire_seed(&f, 2) != tr_scene_wire_seed(&g, 2));

		/* c. A wire frame draws more than the same frame with plain
		 * obstacles, rebuilds bit for bit, animates from frame to frame,
		 * and stays above the board. */
		static tr_dl_t d0, d1;
		tr_frame_in_t  plain = tr_scene_golden_in(1234, 1);
		float          lowest = 1e30f;

		tr_scene_init(&s);
		tr_scene_step(&s, &f);
		tr_scene_build(&s, &plain, &cam, &d0);
		tr_scene_build(&s, &f, &cam, &dl);
		tr_scene_build(&s, &f, &cam, &d1);
		printf("wires: golden run %u tris, with every obstacle a live wire %u\n", d0.n, dl.n);
		assert(dl.n > d0.n && dl.n == d1.n && memcmp(dl.tri, d1.tri, dl.n * sizeof(dl.tri[0])) == 0);
		g = f, g.tick++;
		tr_scene_build(&s, &g, &cam, &d1);
		assert(dl.n != d1.n || memcmp(dl.tri, d1.tri, dl.n * sizeof(dl.tri[0])) != 0);
		for (uint32_t t = 0; t < 64; t++) {
			g = wires_in(1234 + t);
			tr_scene_step(&s, &g);
			tr_scene_build(&s, &g, &cam, &dl);
			float y = min_world_y(&dl, &cam);

			lowest = y < lowest ? y : lowest;
		}
		assert(lowest > -1.5f);

		/* d. Worst case with wires on screen: step.c keeps at most 6
		 * entities live (a spawn every TR_SPAWN_TICKS 20 ticks, 11 px a
		 * tick, freed at the 1280-px track end: <= 5.8), so as a superset 8 live wires of both
		 * heights and 8 full-detail resistors, packed in the full-detail
		 * range, mid-jump, a pickup burst -- then the same as a wire
		 * crash frame; inside TR_DL_MAX_TRIS and every band's bin
		 * (render() asserts it). */
		tr_frame_in_t w = tr_scene_golden_in(4321, 0);
		uint32_t      b0, b1;

		for (int i = 0; i < 16; i++) {
			w.ents[i] = (tr_pkt_ent_t){(uint8_t)(i & 1 ? 3 : 1), (uint8_t)(i % 3), (uint8_t)(i & 2 ? 1 : 0), 0,
						   (int16_t)(900 + 16 * i), 0};
		}
		tr_scene_init(&s);
		tr_scene_step(&s, &w);
		w.lane = 2;
		w.score += 10;
		w.flags |= TR_FLAG_AIRBORNE;
		w.air_ticks = TR_AIR_TICKS / 2;
		tr_scene_step(&s, &w);
		tr_scene_build(&s, &w, &cam, &dl);
		b0 = render(&cam, &w);
		assert(dl.n < TR_DL_MAX_TRIS && tr_dl_dropped == 0);
		printf("wires worst: %u tris, max band bin %u of %u", dl.n, (unsigned)b0, TR_BIN_MAX);
		w.flags      = TR_FLAG_CRASH;
		w.crash_ent  = 15; /* a wire */
		w.crash_lane = w.ents[15].lane;
		w.crash_kind = TR_CRASH_KIND_WIRE;
		tr_scene_step(&s, &w);
		tr_scene_build(&s, &w, &cam, &dl);
		b1 = render(&cam, &w);
		assert(dl.n < TR_DL_MAX_TRIS && tr_dl_dropped == 0);
		printf("; crashed %u tris, max band bin %u\n", dl.n, (unsigned)b1);
		assert(b0 < TR_BIN_MAX && b1 < TR_BIN_MAX);

		/* e. The electric crash: a blue-white flash (bluer, not redder,
		 * than the plain background), blue/white sparks, and the runner
		 * shuddering where a plain crash does not. */
		tr_bg_t          plain_bg, fl;
		tr_frame_in_t    cw = crash_in(0, 2), cl = crash_in(3, 0);
		tr_runner_draw_t r2;

		crash_run(&s, 0, 2, &cam, &dl);
		tr_scene_bg(&cw, &cam, &plain_bg);
		fl = plain_bg;
		tr_scene_bg_flash(&cw, &fl);
		assert((fl.bot & 0x1F) > (plain_bg.bot & 0x1F) && (fl.bot >> 11) < 30);
		assert(dl.tri[dl.n - 1].a[0].w == 0xFFFF && (dl.tri[dl.n - 1].c & 0x1F) > 25);
		for (int i = 0; i < TR_PARTICLES; i++) {
			assert(s.p[i].col == 7 || s.p[i].col == 11 || s.p[i].col == 12);
		}
		crash_run(&s, 3, 2, &cam, &dl);
		cw = crash_in(3, 2);
		tr_scene_runner(&s, &cw, &rd);
		crash_run(&s, 3, 0, &cam, &dl);
		tr_scene_runner(&s, &cl, &r2);
		assert(rd.inst.pos.x != r2.inst.pos.x && rd.inst.yaw != r2.inst.yaw);
		crash_run(&s, 40, 2, &cam, &dl);
		cw = crash_in(40, 2);
		tr_scene_runner(&s, &cw, &rd);
		crash_run(&s, 40, 0, &cam, &dl);
		cl = crash_in(40, 0);
		tr_scene_runner(&s, &cl, &r2);
		assert(rd.inst.pos.x == r2.inst.pos.x); /* the shudder is over */
		printf("wires: arc deterministic per seed, re-seeded per frame, above the board, electric crash\n");
	}

	/* Crash review frames: crash ticks 0 (the hit), 4 and 10 (slow
	 * motion), 20, 40; low obstacle, then a high one at 6. */
	if (getenv("TR_DUMP")) {
		static const uint8_t ct[6]  = {0, 4, 10, 20, 40, 6};
		char                 path[64];

		for (int f = 0; f < 6; f++) {
			tr_frame_in_t c = crash_in(ct[f], f < 5);

			crash_run(&s, ct[f], f < 5, &cam, &dl);
			render(&cam, &c);
			snprintf(path, sizeof(path), "scene-crash-%02u%s", ct[f], f < 5 ? "" : "-high");
			dump(path);
		}
	}

	/* Live-wire review frames (P4b): a high and a low wire in the runner's
	 * lane close up and far off, and the wire crash. */
	if (getenv("TR_DUMP")) {
		static const struct {
			const char *name;
			int16_t     y;
			uint8_t     low;
		} v[4] = {{"wire-high-close", 1020, 0}, {"wire-low-close", 1000, 1}, {"wire-high-far", 800, 0},
			  {"wire-low-far", 800, 1}};

		for (int f = 0; f < 4; f++) {
			tr_frame_in_t a = tr_scene_golden_in(1234, 1);

			a.ents[7] = (tr_pkt_ent_t){3, 1, v[f].low, 0, v[f].y, 0};
			a.ents[4] = (tr_pkt_ent_t){3, f & 1 ? 0 : 2, (uint8_t)!v[f].low, 0, (int16_t)(v[f].y - 60), 0};
			tr_scene_init(&s);
			tr_scene_step(&s, &a);
			tr_scene_build(&s, &a, &cam, &dl);
			render(&cam, &a);
			dump(v[f].name);
		}
		for (int f = 0; f < 4; f++) {
			static const uint8_t ct[4] = {0, 3, 8, 16};
			char                 path[64];
			tr_frame_in_t        c = crash_in(ct[f], f < 3 ? 2 : 3);

			crash_run(&s, ct[f], f < 3 ? 2 : 3, &cam, &dl);
			render(&cam, &c);
			snprintf(path, sizeof(path), "scene-crash-wire-%02u%s", ct[f], f < 3 ? "" : "-low");
			dump(path);
		}
	}

	/* 11. P16: the characters, their reactions, the idle set, the scarf. */
	{
		static tr_scene_t s2;
		static tr_runner_draw_t r2;
		const int     n40 = 40 * 4, r = TR_ANIM_SIDE0 + TR_ANIM_SIDE_N; /* 4 s at 40 Hz; right-side channels */
		float         worst_front = -1e30f, worst_lo = 1e30f, worst_len = 0.0f, worst_plane = -1e30f, rate = 0.0f;
		float         scarf_front = -1e30f, scarf_rate = 0.0f, p16_gap = 0.0f;

		for (int c = 0; c < TR_CHARS; c++) {
			const tr_rig_char_t *rc = &tr_rig_chars[c];
			float                len = rc->scarf[4];
			int                  n = (int)rc->scarf[3];

			tr_scene_init(&s);
			tr_scene_init(&s2);
			for (int f = 0; f <= n40; f++) {
				double        t  = f / 40.0;
				tr_frame_in_t a  = p16_in(t, 40, c);
				float         fr, lo;

				tr_scene_step(&s, &a);
				tr_scene_step(&s2, &a);
				assert(s.chr == c);
				/* a. deterministic: the same stream, the same state */
				assert(memcmp(s.ch, s2.ch, sizeof(s.ch)) == 0 && memcmp(s.sc, s2.sc, sizeof(s.sc)) == 0);
				assert(memcmp(&s.face, &s2.face, sizeof(s.face)) == 0 && s.yaw == s2.yaw);
				tr_scene_runner(&s, &a, &rd);
				tr_scene_runner(&s2, &a, &r2);
				assert(memcmp(rd.xyz, r2.xyz, sizeof(rd.xyz)) == 0 && memcmp(rd.sxyz, r2.sxyz, sizeof(rd.sxyz)) == 0);
				/* b. the runner as drawn: on the board, its front inside
				 * the contact bound through every reaction, spin and the
				 * turn to the camera */
				runner_extent(&rd, &fr, &lo);
				worst_front = fr > worst_front ? fr : worst_front;
				/* P16b: joints closed through every reaction, spin and the idle set */
				rchr = c, rlod = 0;
				skin(s.ch);
				p16_gap = rig_gap(s.ch) > p16_gap ? rig_gap(s.ch) : p16_gap;
				worst_lo    = lo < worst_lo ? lo : worst_lo;
				/* c. the scarf: n segments of its length, never in front
				 * of the neck, every drawn point on or above the board */
				assert(s.scarf_n == n && rd.scarf[0].nt == 2 * n && rd.scarf[1].nt == 2 * n);
				for (int i = 1; i <= n; i++) {
					float d = 0.0f, fw;

					for (int q = 0; q < 3; q++) {
						d += (s.sc[i][q] - s.sc[i - 1][q]) * (s.sc[i][q] - s.sc[i - 1][q]);
					}
					d = sqrtf(d) / len - 1.0f;
					worst_len = fabsf(d) > worst_len ? fabsf(d) : worst_len;
					fw        = (s.sc[i][0] - s.sc[0][0]) * sinf(rd.inst.yaw) + (s.sc[i][2] - s.sc[0][2]) * cosf(rd.inst.yaw);
					worst_plane = fw > worst_plane && t < 1.8 ? fw : worst_plane; /* not mid-spin: the plane turns */
				}
				for (int v = 0; v < 2 * (n + 1); v++) {
					float z = rd.sxyz[v * 3 + 2] - (float)TR_PROJ_Z_RUNNER;

					assert(rd.sxyz[v * 3 + 1] >= 0.0f);
					scarf_front = z > scarf_front ? z : scarf_front; /* mid-spin included */
				}
				if (f == 40) { /* running at the play pace: the scarf streams back */
					assert(s.sc[n][2] < s.sc[0][2] - 0.55f * len * (float)n);
				}
				/* d. the reactions, from the packet */
				if (f == 24) { /* 0.3 s into the pump: the right fist up */
					assert(s.w_react > 0.9f && s.ch[r + 4] < -100.0f && s.face.happy > 0.9f);
				}
				if (f == 50) { /* 0.35 s into the glance, to the left: head and chest turned left */
					assert(s.w_react > 0.9f && s.ch[7] < -40.0f && s.ch[5] < -15.0f && s.face.look_x < -2.0f);
				}
				if (f == 60) { /* 0.2 s into the near miss: pitched forward, squinting */
					assert(s.w_react > 0.9f && s.ch[1] > 16.0f && s.face.squint > 0.9f);
				}
				if (f == 84) { /* 0.3 s into the combo: mid-spin, happy */
					assert(s.yaw > 1.0f && s.yaw < 2.0f * PI_T - 1.0f && s.face.happy > 0.9f);
				}
				if (f == 106) { /* all over: the run alone */
					assert(s.w_react == 0.0f && s.yaw == 0.0f);
				}
				/* e. the lobby: turned to the camera, standing on the board,
				 * the scarf hanging */
				if (f == n40) {
					float fr2, lo2;

					assert(s.w_idle == 1.0f && s.yaw > PI_T - 0.01f && s.yaw < PI_T + 0.01f);
					runner_extent(&rd, &fr2, &lo2);
					assert(lo2 > -0.01f && lo2 < 1.0f && rd.board_lift < 1.0f);
					assert(s.sc[n][1] < s.sc[0][1] - 0.5f * len * (float)n);
				}
			}
			/* f. frame-rate independence of the pose (P3d): 30 vs 40 Hz at
			 * the common times -- the reactions and the idle set are
			 * functions of the packet's clocks, the blends eased per real time */
			tr_scene_init(&s2);
			tr_scene_init(&s);
			for (int f = 0, g = 0; f <= 30 * 4; f++) {
				tr_frame_in_t a = p16_in(f / 30.0, 30, c);

				tr_scene_step(&s2, &a);
				if (f % 3 == 0) {
					for (; g <= f / 3 * 4; g++) {
						tr_frame_in_t b = p16_in(g / 40.0, 40, c);

						tr_scene_step(&s, &b);
					}
					for (int k = 0; k < TR_ANIM_CH; k++) {
						float d = fabsf(s.ch[k] - s2.ch[k]);

						rate = d > rate ? d : rate;
					}
					rate = fabsf(s.yaw - s2.yaw) * 57.3f > rate ? fabsf(s.yaw - s2.yaw) * 57.3f : rate;
					/* the scarf: 120 Hz sub-steps, 4 a 30 Hz frame, 3 a 40 Hz one */
					for (int i = 0; i <= (int)tr_rig_chars[c].scarf[3]; i++) {
						for (int q = 0; q < 3; q++) {
							float d = fabsf(s.sc[i][q] - s2.sc[i][q]);

							scarf_rate = d > scarf_rate ? d : scarf_rate;
						}
					}
				}
			}
		}
		/* c2. the scarf through a duck roll and a crash tumble (the neck
		 * goes round by the board, the chain moved with it as drawn):
		 * still on or above the board, and the drawn anchor on the neck */
		float scarf_lo = 1e30f, sim_lo = 1e30f;

		for (int c = 0; c < TR_CHARS; c++) {
			tr_frame_in_t a = tr_scene_golden_in(1000, 1);

			tr_scene_init(&s);
			a.flags |= TR_FLAG_CHAR;
			a.character = (uint8_t)c;
			for (int f = 0; f < 60; f++, a.tick++) {
				int d = f - 20;

				a.flags      = TR_FLAG_ALIVE | TR_FLAG_CHAR | (d >= 0 && d < TR_DUCK_TICKS - 1 ? TR_FLAG_DUCKING : 0u);
				a.duck_ticks = (uint8_t)(d >= 0 && d < TR_DUCK_TICKS - 1 ? TR_DUCK_TICKS - 1 - d : 0);
				tr_scene_step(&s, &a);
				tr_scene_runner(&s, &a, &rd);
				rchr = c, rlod = 0; /* ... and through the duck roll */
				skin(s.ch);
				p16_gap = rig_gap(s.ch) > p16_gap ? rig_gap(s.ch) : p16_gap;
				for (int v = 0; v < rd.scarf[0].nv; v++) {
					scarf_lo = rd.sxyz[v * 3 + 1] < scarf_lo ? rd.sxyz[v * 3 + 1] : scarf_lo;
				}
				for (int i = 0; i <= s.scarf_n; i++) {
					sim_lo = s.sc[i][1] < sim_lo ? s.sc[i][1] : sim_lo;
				}
			}
			for (uint8_t ct = 0; ct < TR_CRASH_TICKS; ct++) {
				tr_frame_in_t k = crash_in(ct, ct & 1);

				k.flags |= TR_FLAG_CHAR;
				k.character = (uint8_t)c;
				tr_scene_step(&s, &k);
				tr_scene_runner(&s, &k, &rd);
				rchr = c, rlod = 0; /* joints closed at the crash's extremes */
				skin(s.ch);
				p16_gap = rig_gap(s.ch) > p16_gap ? rig_gap(s.ch) : p16_gap;
				for (int v = 0; v < rd.scarf[0].nv; v++) {
					scarf_lo = rd.sxyz[v * 3 + 1] < scarf_lo ? rd.sxyz[v * 3 + 1] : scarf_lo;
				}
				for (int i = 0; i <= s.scarf_n; i++) {
					sim_lo = s.sc[i][1] < sim_lo ? s.sc[i][1] : sim_lo;
				}
			}
		}
		printf("p16: scarf through rolls and crashes: lowest y drawn %.2f, simulated %.2f\n", (double)scarf_lo,
		       (double)sim_lo);
		assert(scarf_lo >= 0.0f);
		assert(sim_lo > 10.0f); /* the sim needs no board constraint (r3d_scene.c scarf_step) */
		printf("p16b: joint gap over reactions, spins, idle, rolls, crashes %.3f (units^2)\n", (double)p16_gap);
		assert(p16_gap < 0.5f);
		rchr = 0;

		printf("p16: front z %.2f (bound %.0f), lowest y %.3f, scarf segment error %.4f, in front of the neck %.3f, "
		       "30 vs 40 Hz pose %.4f deg\n",
		       (double)worst_front, (double)TR_RUNNER_FRONT_Z, (double)worst_lo, (double)worst_len,
		       (double)worst_plane, (double)rate);
		assert(worst_front <= TR_RUNNER_FRONT_Z + 0.01f);
		assert(worst_lo > -0.01f);
		assert(worst_len < 0.001f && worst_plane < 0.01f);
		assert(rate < 0.01f);
		printf("p16: scarf ahead of the runner %.2f (bound %.0f, spins included), 30 vs 40 Hz scarf points %.2f units\n",
		       (double)scarf_front, (double)TR_RUNNER_FRONT_Z, (double)scarf_rate);
		assert(scarf_front <= TR_RUNNER_FRONT_Z);
		assert(scarf_rate < SCARF_RATE_TOL);

		/* h. The P16 fields are read only with TR_FLAG_CHAR: an old HE
		 * never writes them, and in the dev loop they can hold an earlier
		 * HE's bytes -- a stale COMBO must not spin a Probe run, a stale
		 * id must not change the character. */
		{
			tr_frame_in_t a = tr_scene_golden_in(1234, 1);

			tr_scene_init(&s);
			a.character = 3, a.react = TR_REACT_COMBO, a.react_ms = 200, a.react_seq = 2, a.react_side = 1;
			for (int f = 0; f < 10; f++, a.tick++) {
				tr_scene_step(&s, &a);
				assert(s.chr == 0 && s.w_react == 0.0f && s.yaw == 0.0f && s.face.happy == 0.0f);
			}
		}


		/* g. the character in the packet picks the meshes: each draws its
		 * own runner; no TR_FLAG_CHAR (an old HE) or an id out of range:
		 * Probe. */
		{
			tr_frame_in_t a = tr_scene_golden_in(1234, 1);
			uint16_t      n[TR_CHARS + 2];

			for (int c = 0; c < TR_CHARS + 2; c++) {
				tr_frame_in_t b = a;

				if (c < TR_CHARS + 1) {
					b.flags |= TR_FLAG_CHAR;
					b.character = (uint8_t)(c < TR_CHARS ? c : 7);
				} else {
					b.character = 2; /* ignored: no flag */
				}
				tr_scene_init(&s);
				tr_scene_step(&s, &b);
				tr_scene_build(&s, &b, &cam, &dl);
				n[c] = dl.n;
				if (c == TR_CHARS + 1) {
					assert(memcmp(dl.tri, dl2.tri, sizeof(dl.tri[0]) * dl.n) == 0);
				}
				if (c == 0) {
					dl2 = dl;
				}
			}
			printf("p16: golden frame tris per character: %u %u %u %u (id 7 -> %u, no flag -> %u)\n", n[0], n[1], n[2],
			       n[3], n[4], n[5]);
			for (int c = 1; c < TR_CHARS; c++) {
				assert(n[c] != n[0]);
			}
			assert(n[4] == n[0] && n[5] == n[0]);
		}
	}

	/* Art review frames: a lane change mid-bank, a jump, a pickup burst. */
	if (getenv("TR_DUMP")) {
		static const char *const path[3] = {"scene-0", "scene-1", "scene-2"};

		for (int f = 0; f < 3; f++) {
			tr_frame_in_t a = tr_scene_golden_in(200 + 97 * (uint32_t)f, 1);

			tr_scene_init(&s);
			tr_scene_step(&s, &a);
			a.tick++;
			if (f == 0) {
				a.lane = 0; /* two frames into a move left */
				tr_scene_step(&s, &a);
			} else if (f == 1) {
				a.flags |= TR_FLAG_AIRBORNE;
				a.air_ticks = TR_AIR_TICKS / 2;
				a.lane      = 2;
				for (int k = 0; k < 6; k++) {
					tr_scene_step(&s, &a);
				}
			} else {
				a.score += 10;
				tr_scene_step(&s, &a);
				a.tick++;
				tr_scene_step(&s, &a);
			}
			tr_scene_step(&s, &a);
			tr_scene_build(&s, &a, &cam, &dl);
			render(&cam, &a);
			dump(path[f]);
		}
	}

	/* Booth crash shake (tr_mbox.h TR_FLAG_SHAKE): a new HE sends the
	 * amplitude, the scene moves the camera by it and by nothing else; an
	 * old HE's crash packet (no flag) keeps the renderer's own crash_tick
	 * shake, bit for bit; shake 0 under the flag is a still camera. */
	{
		tr_scene_t    ss;
		tr_cam_t      c0, c1;
		static tr_dl_t d0, d1;
		float         dx, dy;
		int           moved = 0, legacy = 0;

		tr_scene_init(&ss);
		for (uint8_t ct = 0; ct < 30u; ct++) {
			tr_frame_in_t old = crash_in(ct, 1), still = old, big = old;

			still.flags |= TR_FLAG_SHAKE;
			still.shake = 0u;
			big.flags |= TR_FLAG_SHAKE;
			big.shake = 255u;
			tr_scene_shake(&old, &dx, &dy); /* legacy: the renderer's own 14 px fade */
			assert(fabsf(dx) <= 14.0f && fabsf(dy) <= 14.0f * 0.7f);
			assert(ct != 29u || (dx == 0.0f && dy == 0.0f)); /* over by then */
			legacy += fabsf(dx) > 1.0f;
			tr_scene_shake(&still, &dx, &dy);
			assert(dx == 0.0f && dy == 0.0f);
			tr_scene_shake(&big, &dx, &dy);
			assert(fabsf(dx) <= TR_SCENE_SHAKE_PX && fabsf(dy) <= TR_SCENE_SHAKE_PX);
			moved += fabsf(dx) > 1.0f;
			/* the build applies exactly that to the camera's centre */
			tr_scene_build(&ss, &still, &c0, &d0);
			tr_scene_build(&ss, &big, &c1, &d1);
			assert(c1.cx == c0.cx + dx && c1.cy == c0.cy + dy && c1.f_px == c0.f_px);
		}
		assert(moved >= 10 && legacy >= 5);
		/* a live frame: no crash, no flag, no shake */
		tr_frame_in_t live = tr_scene_golden_in(1234, 1);

		tr_scene_shake(&live, &dx, &dy);
		assert(dx == 0.0f && dy == 0.0f);
		printf("r3d_scene: crash shake up to %.0f px via the packet, old-HE crash shake kept\n",
		       (double)TR_SCENE_SHAKE_PX);
	}
	return 0;
}
