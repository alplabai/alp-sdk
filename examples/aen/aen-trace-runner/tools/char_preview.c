/* tools/char_preview.c -- the P16 characters for art review, host-rendered
 * through the real scene + band raster. Not a test.
 *
 *   cc -std=c11 -O2 -ffp-contract=off -o /tmp/char_preview tools/char_preview.c \
 *      src/render/proj.c src/render/sprite.c src/render/r3d_math.c src/render/r3d_raster.c \
 *      src/render/r3d_scene.c src/render/r3d_rig.c -lm
 *   /tmp/char_preview OUTDIR stills    -> OUTDIR/<name>-front.ppm, <name>-game.ppm per character
 *   /tmp/char_preview OUTDIR reel CHR  -> OUTDIR/reel-NNN.ppm: 30 fps, a scripted run with every
 *                                         reaction, then the attract lobby (idle set)
 *
 * The packets are built the way the HE builds them at a 30 Hz panel (tick +
 * sub-tick phase at the 0.5x play pace, TR_FLAG_CHAR, react_* / idle_ms). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/render/meshes.h"
#include "../src/render/proj.h"
#include "../src/render/r3d_scene.h"

#define W TR_R3D_W
#define H TR_R3D_H

static uint16_t       fb[W * H];
static uint16_t       zb[W * TR_BAND_H], cb[W * TR_BAND_H];
static uint16_t       bins[TR_BANDS][TR_BIN_MAX];
static uint32_t       counts[TR_BANDS];
static tr_tri_setup_t setup[TR_DL_MAX_TRIS];
static tr_dl_t        dl;

static void raster(const tr_cam_t *cam, const tr_frame_in_t *in)
{
	uint32_t overflow = 0;
	tr_bg_t  bg;

	tr_scene_bg(in, cam, &bg);
	tr_scene_bg_flash(in, &bg);
	tr_bin_build(&dl, setup, bins, counts, &overflow);
	for (int b = 0; b < TR_BANDS; b++) {
		tr_raster_band(
		    fb, W, b * TR_BAND_H, (b + 1) * TR_BAND_H, zb, cb, &bg, &dl, setup, bins[b], counts[b]);
	}
}

static void dump(const char *path)
{
	FILE *f = fopen(path, "wb");

	if (!f) {
		perror(path);
		exit(1);
	}
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; i++) {
		uint16_t p      = fb[i];
		uint8_t  rgb[3] = { (uint8_t)((p >> 11) << 3),
			                (uint8_t)(((p >> 5) & 63) << 2),
			                (uint8_t)((p & 31) << 3) };

		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

/* The HE's packet at real time t s (30 Hz panel, play pace). */
static tr_frame_in_t packet(double t, int chr)
{
	static const struct {
		uint8_t kind, lane, low;
		int16_t y;
	} e[]            = { { 1, 0, 1, 40 },  { 2, 1, 0, 150 }, { 1, 2, 0, 300 },
		                 { 2, 0, 0, 420 }, { 1, 2, 1, 700 }, { 2, 2, 0, 820 } };
	tr_frame_in_t in = { 0 };
	double        g  = t * 20.0 + 1e-9;
	uint32_t      el = (uint32_t)g;

	in.tick      = 2000 + el;
	in.phase     = (uint16_t)((g - (double)el) * 65536.0);
	in.flags     = TR_FLAG_ALIVE | TR_FLAG_CHAR | (in.phase ? TR_FLAG_PHASE : 0u);
	in.lane      = 1;
	in.score     = 120;
	in.hz        = 30;
	in.pace_q8   = (uint8_t)(TR_PLAY_SPEED_Q16 * 40u / 30u >> 8);
	in.character = (uint8_t)chr;
	in.react     = TR_REACT_NONE;
	in.react_ms  = 65535;
	for (unsigned i = 0; i < sizeof(e) / sizeof(e[0]); i++) {
		int y = e[i].y + (int)(el % 120u) * TR_SCROLL_PX; /* keep the road populated */

		in.ents[i] = (tr_pkt_ent_t){ e[i].kind, e[i].lane, e[i].low, 0, (int16_t)(y % 1100), 0 };
	}
	return in;
}

static void react_at(tr_frame_in_t *in, double t, double t0, uint8_t kind, int side, uint8_t seq)
{
	if (t >= t0) {
		in->react      = kind;
		in->react_side = (uint8_t)(int8_t)side;
		in->react_seq  = seq;
		in->react_ms   = (uint16_t)((t - t0) * 1000.0);
	}
}

/* A turntable shot of the runner alone (front 3/4, from its left front),
 * on a plain board. */
static void shot(const tr_scene_t *s, const tr_frame_in_t *in, tr_v3_t eye, tr_v3_t at, float f_px)
{
	tr_runner_draw_t r;
	tr_cam_t         cam;
	const tr_light_t l = {
		{ 0.46f, 0.78f, -0.42f }, 0.38f, { 206, 112, 72 }, 600.0f, 8800.0f, NULL, 0.0f, 0.0f
	};
	float dx = at.x - eye.x, dy = at.y - eye.y, dz = at.z - eye.z;

	tr_scene_runner(s, in, &r);
	tr_cam_build(&cam, eye, atan2f(dx, dz), atan2f(-dy, sqrtf(dx * dx + dz * dz)), 0.0f, f_px);
	dl.n = 0;
	{
		float   z0 = (float)TR_PROJ_Z_RUNNER - 400.0f, z1 = (float)TR_PROJ_Z_RUNNER + 400.0f;
		tr_v3_t q[4] = { { -500, 0, z0 }, { -500, 0, z1 }, { 500, 0, z1 }, { 500, 0, z0 } };

		tr_r3d_emit_quad(&dl, &cam, q, 0x2B4A, TR_TRI_NOZ);
	}
	r.inst.pos.x -= s->runner_x;
	for (int p = 0; p < TR_RIG_DRAWN; p++) {
		r.inst.mesh = &r.mesh[p];
		tr_r3d_emit_mesh(&dl, &cam, &l, &r.inst);
	}
	for (int k = 0; k < 2; k++) {
		for (int i = 0; i < r.scarf[k].nv; i++) {
			r.sxyz[i * 3] -= k == 0 ? s->runner_x : 0.0f;
		}
		r.sinst.mesh = &r.scarf[k];
		if (r.scarf[k].nt) {
			tr_r3d_emit_mesh(&dl, &cam, &l, &r.sinst);
		}
	}
	raster(&cam, in);
}

static void front_shot(const tr_scene_t *s, const tr_frame_in_t *in)
{
	shot(s,
	     in,
	     (tr_v3_t){ -250.0f, 170.0f, (float)TR_PROJ_Z_RUNNER + 330.0f },
	     (tr_v3_t){ 0.0f, 95.0f, (float)TR_PROJ_Z_RUNNER },
	     1500.0f);
}

/* The game camera's direction (behind, above, 18 deg down) zoomed so the
 * runner fills about a third of the frame: what a booth visitor sees, closer. */
static void back_shot(const tr_scene_t *s, const tr_frame_in_t *in)
{
	shot(s,
	     in,
	     (tr_v3_t){ 0.0f, 400.0f, (float)TR_PROJ_Z_RUNNER - 360.0f },
	     (tr_v3_t){ 0.0f, 100.0f, (float)TR_PROJ_Z_RUNNER + 20.0f },
	     1250.0f);
}

int main(int argc, char **argv)
{
	char              path[512];
	static tr_scene_t s;
	tr_cam_t          cam;

	if (argc < 3) {
		fprintf(stderr, "usage: %s OUTDIR stills|reel [CHR]\n", argv[0]);
		return 2;
	}
	if (strcmp(argv[2], "stills") == 0) {
		for (int c = 0; c < TR_CHARS; c++) {
			tr_frame_in_t in;

			/* game camera, mid-run after a second of running */
			tr_scene_init(&s);
			for (int f = 0; f <= 30; f++) {
				in = packet(f / 30.0, c);
				tr_scene_step(&s, &in);
			}
			tr_scene_build(&s, &in, &cam, &dl);
			raster(&cam, &in);
			snprintf(path, sizeof(path), "%s/%s-game.ppm", argv[1], tr_rig_chars[c].name);
			dump(path);
			/* front 3/4, standing idle (facing the track, eyes open) */
			tr_scene_init(&s);
			for (int f = 0; f <= 12; f++) {
				in = packet(0.0, c);
				in.flags |= TR_FLAG_IDLE;
				in.idle_ms = 0;
				tr_scene_step(&s, &in);
			}
			front_shot(&s, &in);
			snprintf(path, sizeof(path), "%s/%s-front.ppm", argv[1], tr_rig_chars[c].name);
			dump(path);
			/* front 3/4 mid-run */
			tr_scene_init(&s);
			for (int f = 0; f <= 34; f++) {
				in = packet(f / 30.0, c);
				tr_scene_step(&s, &in);
			}
			front_shot(&s, &in);
			snprintf(path, sizeof(path), "%s/%s-front-run.ppm", argv[1], tr_rig_chars[c].name);
			dump(path);
			/* front 3/4, 0.3 s into a pickup's fist pump */
			for (int f = 35; f <= 44; f++) {
				in = packet(f / 30.0, c);
				react_at(&in, f / 30.0, 35 / 30.0, TR_REACT_PICKUP, 1, 1);
				tr_scene_step(&s, &in);
			}
			front_shot(&s, &in);
			snprintf(path, sizeof(path), "%s/%s-front-pump.ppm", argv[1], tr_rig_chars[c].name);
			dump(path);
		}
		return 0;
	}
	if (strcmp(argv[2], "closeup") == 0) {
		/* per character: back (game angle) and front 3/4 mid-run, and back
		 * views at the pose extremes -- a duck roll, a crash, a spin */
		for (int c = 0; c < TR_CHARS; c++) {
			tr_frame_in_t            in;
			static const char *const nm[5] = {
				"back-run", "front-run", "back-roll", "back-crash", "back-spin"
			};

			for (int v = 0; v < 5; v++) {
				tr_scene_init(&s);
				for (int f = 0; f <= 40; f++) {
					in = packet(f / 30.0, c);
					if (v == 2 && f >= 30) {
						in.flags |= TR_FLAG_DUCKING;
						in.duck_ticks = (uint8_t)(TR_DUCK_TICKS - 1 - (f - 30) * 2 / 3);
					}
					if (v == 3 && f >= 34) {
						in.flags      = TR_FLAG_CRASH | TR_FLAG_CHAR;
						in.crash_tick = (uint8_t)((f - 34) * 4 / 3 + 6);
						in.crash_kind = TR_CRASH_KIND_LOW, in.crash_lane = 1, in.crash_ent = 15;
					}
					react_at(
					    &in, f / 30.0, 30 / 30.0, v == 4 ? TR_REACT_COMBO : TR_REACT_NONE, 1, 1);
					tr_scene_step(&s, &in);
				}
				if (v == 1) {
					front_shot(&s, &in);
				} else {
					back_shot(&s, &in);
				}
				snprintf(path, sizeof(path), "%s/%s-%s.ppm", argv[1], tr_rig_chars[c].name, nm[v]);
				dump(path);
			}
		}
		return 0;
	}
	/* reel: pickup pump 0.5 s, glance back 1.5 s (right), stumble 2.7 s, combo spin 3.7 s,
	 * then the lobby from 5.0 s (idle set: turn, wave, look around, stretch) */
	int chr = argc > 3 ? atoi(argv[3]) : 0;

	tr_scene_init(&s);
	for (int f = 0; f < 30 * 17; f++) {
		double        t  = f / 30.0;
		tr_frame_in_t in = packet(t < 5.0 ? t : 5.0, chr);

		react_at(&in, t, 0.5, TR_REACT_PICKUP, 1, 1);
		react_at(&in, t, 1.5, TR_REACT_PASS, 1, 2);
		react_at(&in, t, 2.7, TR_REACT_NEAR, -1, 3);
		react_at(&in, t, 3.7, TR_REACT_COMBO, 1, 4);
		if (t >= 5.0) {
			in.flags |= TR_FLAG_IDLE;
			in.flags &= ~TR_FLAG_PHASE;
			in.phase   = 0;
			in.idle_ms = (uint16_t)((t - 5.0) * 1000.0);
			for (int i = 0; i < 16; i++) {
				in.ents[i].kind = 0;
			}
		}
		tr_scene_step(&s, &in);
		tr_scene_build(&s, &in, &cam, &dl);
		raster(&cam, &in);
		snprintf(path, sizeof(path), "%s/reel-%03d.ppm", argv[1], f);
		dump(path);
	}
	return 0;
}
