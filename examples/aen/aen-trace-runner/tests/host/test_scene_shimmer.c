/* tests/host/test_scene_shimmer.c -- ground shimmer in every zone: the golden
 * run at 30 Hz (game pace 0.5x), running straight and changing lane (the
 * camera sliding across), each frame rendered the A32 way and
 * as a 16x supersampled ground truth (the DL shifted by 4 x 4 quarter-pixel
 * offsets, averaged). A pixel's aliasing error e = render - truth
 * (luminance); a flicker pixel is one whose e jumps by >= FLIP between two
 * frames. Counted on the ground only, split into the lanes (|x| < 356 world)
 * and the shoulders (364 < |x| < 836), each near (250..700 deep), mid
 * (700..2200) and far (2200..6000). The board zone's shoulders, mid, were the
 * first complaint (r3d_scene.c fade_detail(): 13.5 % -> 5.2 % of those
 * pixels a frame); memory canyon's near bus lines the next ("the bottom road
 * grid is flickering"). Every zone, both motions, every band now holds the
 * accepted board level (shimmer_max[]) -- toy art (genzone.py --preset toy)
 * measures well under it everywhere; control_shimmer() re-points the near
 * lane ground slot at a synthetic high-contrast checker and runs it through
 * the real pipeline, proving the flip rule itself still fires independent
 * of the art preset in use (see its own comment). Host only (~4,000
 * renders; not under qemu). TR_SHIMMER_PRINT_ONLY: report, assert nothing. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/game/state.h"
#include "../../src/render/r3d_scene.h"
#include "../../src/game/zone.h"
#include "tr_scene_golden.h"

#define W    TR_R3D_W
#define H    TR_R3D_H
#define NF   24
#define FLIP 32.0f
/* The accepted board levels, near / mid / far: its shoulders near 8.45 %
 * (magnified, raw texels: every zone's art >= 2 texels a feature keeps to
 * it), mid 5.2 % after fade_detail() (was 13.5), far well under. */
static const double shimmer_max[3] = {9.0, 7.0, 7.0};

static uint16_t       fb[W * H], idb[W * H];
static uint16_t       zb[W * TR_BAND_H], cb[W * TR_BAND_H];
static uint16_t       bins[TR_BANDS][TR_BIN_MAX];
static uint32_t       counts[TR_BANDS];
static tr_tri_setup_t setup[TR_DL_MAX_TRIS];
static tr_dl_t        dl, dls;
static float          truth[W * H], e0[W * H];

static void raster(uint16_t *f, const tr_dl_t *d, const tr_bg_t *bg)
{
	uint32_t ov = 0;

	tr_bin_build(d, setup, bins, counts, &ov);
	for (int b = 0; b < TR_BANDS; b++) {
		tr_raster_band(f, W, b * TR_BAND_H, (b + 1) * TR_BAND_H, zb, cb, bg, d, setup, bins[b], counts[b]);
	}
}

static float lum(uint16_t p)
{
	return 0.299f * (float)((p >> 11) << 3) + 0.587f * (float)(((p >> 5) & 63) << 2) + 0.114f * (float)((p & 31) << 3);
}

/* Control: proves the flip rule (e = render - truth; flip when |e - e0| >=
 * FLIP) still fires through the REAL pipeline -- tr_scene_build(),
 * tr_bin_build(), tr_raster_band(), the actual camera and the same 4 x 4
 * jitter truth run() uses -- not a standalone reimplementation off to the
 * side, which would stay green even if e.g. the jitter or the camera
 * motion broke and the real run() counts went to zero. The toy art's board
 * shoulders near dropped from 8.45 % to well under 1 % (flat shapes,
 * blurred: shimmer is the point of the preset), so a fixed zone/band can no
 * longer stand in for "does the metric still work" -- instead this re-points
 * the near lane ground slot (TR_SCENE_TEX_LANE) at a synthetic hard 2-texel
 * checkerboard every frame (bind_ground(), called inside tr_scene_build(),
 * detects the re-point and puts zone 0's own lane art straight back on the
 * *next* call -- see r3d_scene.c's bind_ground() comment "and nothing
 * re-pointed a slot since (a test, the golden DL)" -- so this overwrites it
 * again after every tr_scene_build(), same as test_r3d_sky.c's ground-fog
 * case), then runs the identical raster / 16x-SSAA-truth / flip-count as
 * run(), restricted to lanes/near (250..700 deep, |x| < 356: the checker's
 * own band). fog_start/end/knee/far match r3d_scene.c's FOG_START/FOG_END/
 * FOG_KNEE/FOG_FAR exactly (private #defines there) so tr_r3d_fog_build()'s
 * tr_r3d_fog_lut_build() call rebuilds the identical global LUT tr_scene_init()
 * already built -- a no-op past this slot, not a second fog curve. */
static uint16_t ctrl_tex[TR_TEX_DIM * TR_TEX_DIM];
static uint8_t  ctrl_idx[TR_TEX_DIM * TR_TEX_DIM];
static uint16_t ctrl_pal[TR_FOG_LEVELS * TR_FOG_PAL_MAX];

static double control_shimmer(void)
{
	static const tr_light_t l = {{0.0f, 1.0f, 0.0f}, 0.0f, {206, 112, 72}, 600.0f, 8800.0f, NULL, 3600.0f, 72000.0f};
	tr_scene_t               s;
	tr_cam_t                 cam;
	uint64_t                 hit = 0, tot = 0;

	for (uint32_t i = 0; i < TR_TEX_DIM * TR_TEX_DIM; i++) {
		ctrl_tex[i] = (((i % TR_TEX_DIM) >> 1) + ((i / TR_TEX_DIM) >> 1)) & 1u ? 0xffffu : 0x0000u;
	}

	tr_scene_init(&s);
	for (int n = 0; n < NF; n++) {
		tr_frame_in_t in = tr_scene_golden_in(3000, 1);
		double        g  = n * 20.0 / 30.0 + 1e-9;
		uint32_t      el = (uint32_t)g;
		tr_bg_t       bg, b0 = {0};

		in.hz      = 30;
		in.pace_q8 = (uint8_t)((((TR_GAME_PACE_Q8 << 8) * 40u + 15u) / 30u) >> 8);
		in.tick    = 3000 + el;
		in.phase   = (uint16_t)((g - (double)el) * 65536.0);
		in.flags   = TR_FLAG_ALIVE | TR_FLAG_ZONE | (in.phase ? TR_FLAG_PHASE : 0u);
		in.zone    = 0;
		in.gate_y  = TR_ZONE_NO_GATE;
		tr_scene_step(&s, &in);
		tr_scene_build(&s, &in, &cam, &dl);
		tr_scene_bg(&in, &cam, &bg);

		tr_r3d_tex[TR_SCENE_TEX_LANE] = ctrl_tex;
		assert(tr_r3d_fog_build(TR_SCENE_TEX_LANE, &l, ctrl_idx, ctrl_pal));

		memset(truth, 0, sizeof(truth));
		for (int j = 0; j < 16; j++) {
			dls = dl;
			for (int t = 0; t < dls.n; t++) {
				for (int k = 0; k < 3; k++) {
					dls.tri[t].v[k].x += (j & 3) * 4 - 6; /* 28.4: +-1/8, +-3/8 px */
					dls.tri[t].v[k].y += (j >> 2) * 4 - 6;
				}
			}
			raster(fb, &dls, &bg);
			for (int i = 0; i < W * H; i++) {
				truth[i] += lum(fb[i]) * (1.0f / 16.0f);
			}
		}
		dls = dl;
		for (int t = 0; t < dls.n; t++) {
			dls.tri[t].c = (dls.tri[t].flags & TR_TRI_NOZ) ? 0 : 1;
			dls.tri[t].flags &= TR_TRI_NOZ;
		}
		raster(idb, &dls, &b0);
		raster(fb, &dl, &bg);

		const float (*m)[4] = cam.view.m;
		float             eye[3];

		for (int i = 0; i < 3; i++) {
			eye[i] = -(m[0][i] * m[0][3] + m[1][i] * m[1][3] + m[2][i] * m[2][3]);
		}
		for (int i = 0; i < W * H; i++) {
			float e = lum(fb[i]) - truth[i];
			float d[3] = {((float)(i % W) + 0.5f - cam.cx) / cam.f_px, -((float)(i / W) + 0.5f - cam.cy) / cam.f_px, 1.0f};
			float dy = m[0][1] * d[0] + m[1][1] * d[1] + m[2][1] * d[2];

			if (n > 0 && !idb[i] && dy < 0.0f) {
				float t  = -eye[1] / dy; /* d.z == 1: t is the view depth */
				float ax = fabsf(eye[0] + t * (m[0][0] * d[0] + m[1][0] * d[1] + m[2][0] * d[2]));

				if (ax < 356.0f && t >= 250.0f && t < 700.0f) {
					tot++;
					hit += fabsf(e - e0[i]) >= FLIP;
				}
			}
			e0[i] = e;
		}
	}
	assert(tot > 20000);
	return 100.0 * (double)hit / (double)tot;
}

/* flips / px per [motion][lanes, shoulders][near, mid, far] */
static uint64_t flips[2][2][3], px[2][2][3];
static const float band_z[4] = {250.0f, 700.0f, 2200.0f, 6000.0f};

static void run(uint8_t zone, int lane_change)
{
	tr_scene_t s;
	tr_cam_t   cam;

	tr_scene_init(&s);
	for (int n = 0; n < NF; n++) {
		tr_frame_in_t in = tr_scene_golden_in(3000, (uint8_t)(lane_change && n >= 4 ? 2 : 1));
		double        g  = n * 20.0 / 30.0 + 1e-9;
		uint32_t      el = (uint32_t)g;
		tr_bg_t       bg, b0 = {0};

		in.hz      = 30;
		in.pace_q8 = (uint8_t)((((TR_GAME_PACE_Q8 << 8) * 40u + 15u) / 30u) >> 8);
		in.tick    = 3000 + el;
		in.phase   = (uint16_t)((g - (double)el) * 65536.0);
		in.flags   = TR_FLAG_ALIVE | TR_FLAG_ZONE | (in.phase ? TR_FLAG_PHASE : 0u);
		in.zone    = zone;
		in.gate_y  = TR_ZONE_NO_GATE;
		tr_scene_step(&s, &in);
		tr_scene_build(&s, &in, &cam, &dl);
		tr_scene_bg(&in, &cam, &bg);

		memset(truth, 0, sizeof(truth));
		for (int j = 0; j < 16; j++) {
			dls = dl;
			for (int t = 0; t < dls.n; t++) {
				for (int k = 0; k < 3; k++) {
					dls.tri[t].v[k].x += (j & 3) * 4 - 6; /* 28.4: +-1/8, +-3/8 px */
					dls.tri[t].v[k].y += (j >> 2) * 4 - 6;
				}
			}
			raster(fb, &dls, &bg);
			for (int i = 0; i < W * H; i++) {
				truth[i] += lum(fb[i]) * (1.0f / 16.0f);
			}
		}
		/* which pixels are ground: the DL with every z-tested tri flat 1, NOZ 0 */
		dls = dl;
		for (int t = 0; t < dls.n; t++) {
			dls.tri[t].c = (dls.tri[t].flags & TR_TRI_NOZ) ? 0 : 1;
			dls.tri[t].flags &= TR_TRI_NOZ;
		}
		raster(idb, &dls, &b0);
		raster(fb, &dl, &bg);

		/* ground hit of each pixel's ray: eye + t * (view^T d), y = 0 */
		const float (*m)[4] = cam.view.m;
		float eye[3];

		for (int i = 0; i < 3; i++) {
			eye[i] = -(m[0][i] * m[0][3] + m[1][i] * m[1][3] + m[2][i] * m[2][3]);
		}
		for (int i = 0; i < W * H; i++) {
			float e = lum(fb[i]) - truth[i];
			float d[3] = {((float)(i % W) + 0.5f - cam.cx) / cam.f_px, -((float)(i / W) + 0.5f - cam.cy) / cam.f_px, 1.0f};
			float dy = m[0][1] * d[0] + m[1][1] * d[1] + m[2][1] * d[2];

			if (n > 0 && !idb[i] && dy < 0.0f) {
				float t  = -eye[1] / dy; /* d.z == 1: t is the view depth */
				float ax = fabsf(eye[0] + t * (m[0][0] * d[0] + m[1][0] * d[1] + m[2][0] * d[2]));
				int   r  = ax < 356.0f ? 0 : ax > 364.0f && ax < 836.0f ? 1 : -1;

				for (int b = 0; r >= 0 && b < 3; b++) {
					if (t >= band_z[b] && t < band_z[b + 1]) {
						px[lane_change][r][b]++;
						flips[lane_change][r][b] += fabsf(e - e0[i]) >= FLIP;
					}
				}
			}
			e0[i] = e;
		}
	}
}

int main(void)
{
	static const char *reg[2] = {"lanes", "shoulders"}, *bnd[3] = {"near", "mid", "far"};
	double             worst = 0.0;

	for (uint8_t zn = 0; zn < TR_ZONES; zn++) {
		memset(flips, 0, sizeof(flips));
		memset(px, 0, sizeof(px));
		run(zn, 0);
		run(zn, 1);
		for (int mo = 0; mo < 2; mo++) {
			printf("shimmer %-13s %-8s", tr_zone_name(zn), mo ? "lane chg" : "straight");
			for (int r = 0; r < 2; r++) {
				for (int b = 0; b < 3; b++) {
					double f = px[mo][r][b] ? 100.0 * (double)flips[mo][r][b] / (double)px[mo][r][b] : 0.0;

					printf(" %s/%s %5.2f %%", reg[r], bnd[b], f);
					/* sample floor, screen px: tuned at TR_VIEW_TUNED_H, scaled
					 * by the viewport's AREA (the picture scales both ways) */
					assert(px[mo][r][b] > TR_VIEW_PX(TR_VIEW_PX(20000)));
					worst = f > worst ? f : worst;
#ifndef TR_SHIMMER_PRINT_ONLY
					assert(f < shimmer_max[b]);
#endif
				}
			}
			printf("\n");
			fflush(stdout);
		}
	}
	/* the control: the flip rule itself still fires (synthetic checkerboard,
	 * independent of the art preset -- see control_shimmer()) */
	double ctrl = control_shimmer();
	assert(ctrl > 4.0);
	printf("shimmer control: synthetic 2-texel checker via the real pipeline %.2f %% (metric still detects flicker)\n", ctrl);
	printf("shimmer: worst %.2f %% (|d(render - 16x SSAA)| >= %.0f; max near %.1f, mid / far %.1f)\n", worst, (double)FLIP,
	       shimmer_max[0], shimmer_max[1]);
	return 0;
}
