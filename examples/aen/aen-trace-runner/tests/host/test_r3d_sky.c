/* tests/host/test_r3d_sky.c -- the dithered sky, sun halo and stars of the
 * band background (tr_bg_t.fx) and the fogged near ground (tr_r3d_fog_build,
 * TR_TRI_NOZ textured spans). Runs on the host and, in runner.sh's A32 stage,
 * under qemu on the NEON paths. See src/render/r3d.h (tr_bg_t, tr_tex_fog_t). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/render/meshes.h"
#include "../../src/render/r3d.h"
#include "../../src/game/zone.h"
#include "../../src/render/r3d_scene.h"
#include "../../src/render/zones.h"

static uint16_t       fb[TR_R3D_W * TR_R3D_H];
static uint16_t       zb[TR_BAND_H * TR_R3D_W], cb[TR_BAND_H * TR_R3D_W];
static uint16_t       bins[TR_BANDS][TR_BIN_MAX];
static uint32_t       counts[TR_BANDS];
static tr_tri_setup_t setup[TR_DL_MAX_TRIS];
static tr_dl_t        dl;

static void render(const tr_bg_t *bg)
{
	uint32_t ov = 0;

	tr_bin_build(&dl, setup, bins, counts, &ov);
	assert(ov == 0);
	for (int b = 0; b < TR_BANDS; b++) {
		tr_raster_band(fb, TR_R3D_W, b * TR_BAND_H, (b + 1) * TR_BAND_H, zb, cb, bg, &dl, setup, bins[b], counts[b]);
	}
}

static uint16_t px(int x, int y)
{
	return fb[y * TR_R3D_W + x];
}

/* RGB565 -> 8-bit channels (the display's expansion). */
static void ch8(uint16_t c, int v[3])
{
	int r = c >> 11, g = (c >> 5) & 63, b = c & 31;

	v[0] = (r << 3) | (r >> 2), v[1] = (g << 2) | (g >> 4), v[2] = (b << 3) | (b >> 2);
}

int main(void)
{
	/* 1. Ordered dither, exact. Red 0 -> 2 (RGB565 units) over rows 0..4:
	 * rows 1 and 3 sit at 0.5 and 1.5, so a pixel rounds up exactly where the
	 * 4x4 Bayer entry is >= 8; rows 0, 2, 4 are whole and do not dither. */
	{
		static const uint8_t r5[5][4] = {{0, 0, 0, 0}, {1, 0, 1, 0}, {1, 1, 1, 1}, {2, 1, 2, 1}, {2, 2, 2, 2}};
		tr_bg_t              bg = {0};

		bg.horizon = 5, bg.top = 0x0000, bg.bot = 0x1000, bg.ground = 0xFFFF, bg.fx = TR_BG_DITHER;
		dl.n = 0;
		render(&bg);
		for (int y = 0; y < 8; y++) {
			for (int x = 0; x < TR_R3D_W; x++) {
				assert(px(x, y) == (y < 5 ? (uint16_t)(r5[y][x & 3] << 11) : 0xFFFF));
			}
		}
		/* Mid stop: 0 -> 2 at half way -> 0 over rows 0..8. */
		bg.horizon = 9, bg.mid = 0x1000, bg.mid_q8 = 128, bg.bot = 0x0000;
		render(&bg);
		for (int x = 0; x < TR_R3D_W; x++) {
			assert(px(x, 2) == 1u << 11 && px(x, 4) == 2u << 11 && px(x, 6) == 1u << 11);
		}
		/* A real gradient: every 4x4 block's mean is the undithered value
		 * to within the block's quantisation (1/16 step), i.e. no band. */
		bg      = (tr_bg_t){0};
		bg.horizon = 640, bg.top = 0x0806, bg.mid = 0x780E, bg.mid_q8 = 150, bg.bot = 0xCB89;
		bg.fx      = TR_BG_DITHER;
		render(&bg);
		for (int y = 0; y + 4 <= 640; y += 4) {
			int s0 = 0, s1 = 0;

			for (int k = 0; k < 16; k++) {
				s0 += px(100 + (k & 3), y + (k >> 2)) >> 11;
				s1 += px(104 + (k & 3), y + (k >> 2)) >> 11;
			}
			assert(s0 == s1); /* the pattern tiles: no column drift */
		}
		printf("dither: exact Bayer pattern, mid stop, tiles\n");
	}

	/* 2. Halo: brighter toward the sun, untouched past its radius, NEON ==
	 * scalar (tr_raster_selfcheck). */
	{
		tr_bg_t bg = {0};
		int     a[3], b[3], c[3];

		assert(tr_raster_selfcheck());
		bg.horizon = 600, bg.top = bg.mid = bg.bot = 0x2104, bg.fx = TR_BG_DITHER;
		bg.sun_x = 360, bg.sun_y = 300, bg.halo_r = 120, bg.halo = 0xFD20;
		dl.n = 0;
		render(&bg);
		ch8(px(360, 300), a);
		ch8(px(360 + 60, 300), b);
		ch8(px(360 + 130, 300), c);
		assert(a[0] > b[0] && b[0] > c[0] && px(360 + 130, 300) == 0x2104 && px(360, 300 - 125) == 0x2104);
		printf("halo: %d > %d > %d (red), none past r\n", a[0], b[0], c[0]);
	}

	/* 3. Stars: deterministic, fixed to the horizon (move with it), none in
	 * the 60 rows above it, fading in with height. Horizon 600: inside the
	 * TR_VIEW_H (640) rows render() rasters. */
	{
		static uint16_t f0[TR_R3D_W * TR_R3D_H];
		tr_bg_t         bg = {0};
		int             n = 0, lo = 0, hi = 0;
		const uint16_t  sky = 0x0843;

		bg.horizon = 600, bg.top = bg.mid = bg.bot = sky, bg.ground = 0, bg.fx = TR_BG_DITHER | TR_BG_STARS;
		dl.n = 0;
		render(&bg);
		memcpy(f0, fb, sizeof(fb));
		render(&bg);
		assert(memcmp(f0, fb, sizeof(fb)) == 0);
		bg.horizon = 610;
		render(&bg);
		for (int y = 0; y < 600; y++) {
			for (int x = 0; x < TR_R3D_W; x++) {
				uint16_t s = f0[y * TR_R3D_W + x];

				assert(s == px(x, y + 10));
				if (s != sky) {
					int v[3];

					n++;
					assert(y < 600 - 58);
					ch8(s, v);
					if (y < 600 - 300) {
						hi += v[0] + v[1] + v[2];
					} else {
						lo += v[0] + v[1] + v[2];
					}
				}
			}
		}
		printf("stars: %d px, fade high %d > low %d\n", n, hi, lo);
		assert(n > 150 && n < 1200 && hi > lo);
	}

	/* 4. Ground fog: a flat-coloured textured NOZ ground to TEX_Z, fogged by
	 * the palette path, then the Gouraud board tile mesh fogged per vertex
	 * (the scene's split). Walking a screen column toward the horizon the
	 * colour moves monotonically toward the fog colour with no step at the
	 * handover. The unfogged ground (the pre-fog raster) steps by ~0.44 of
	 * the way to the fog there. */
	{
		static uint16_t  tex[TR_TEX_DIM * TR_TEX_DIM];
		static uint8_t   idx[TR_TEX_DIM * TR_TEX_DIM];
		static uint16_t  pal[TR_FOG_LEVELS * TR_FOG_PAL_MAX];
		const tr_light_t l = {{0.0f, 1.0f, 0.0f}, 0.0f, {206, 112, 72}, 600.0f, 8800.0f, NULL, 0.0f, 0.0f};
		const float      tex_z = 2688.0f;
		tr_cam_t         cam;
		tr_bg_t          bg  = {0};
		int              fog[3] = {206, 112, 72}, dmax = 0, back = 0, prev = -1;

		for (int i = 0; i < TR_TEX_DIM * TR_TEX_DIM; i++) {
			tex[i] = tr_r3d_palette[2];
		}
		tr_r3d_tex[7] = tex;
		assert(tr_r3d_fog_build(7, &l, idx, pal) && tr_r3d_tex_fog[7].npal == 1);
		for (int i = 1; i < TR_FOG_LUT_N; i++) {
			assert(tr_r3d_fog_lut[i] <= tr_r3d_fog_lut[i - 1]); /* nearer (larger w): less fog */
		}
		tr_cam_build(&cam, (tr_v3_t){0.0f, TR_CAM_EYE_H, -144.0f}, 0.0f, TR_CAM_PITCH_DEG * 3.14159265f / 180.0f, 0.0f,
			     TR_CAM_F_PX);
		dl.n = 0;
		{
			const tr_v3_t  q[4]     = {{-600, 0, -100}, {-600, 0, tex_z}, {600, 0, tex_z}, {600, 0, -100}};
			const uint16_t uv[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};

			assert(tr_r3d_emit_quad_tex(&dl, &cam, q, uv, 7, TR_TRI_NOZ) == 2);
		}
		for (float z = tex_z; z < 8800.0f; z += (float)TR_TILE_LEN) {
			tr_inst_t in = {&tr_mesh_tile_board, {-840.0f, 0.0f, z}, 0.0f, 1.0f, TR_TRI_GOURAUD | TR_TRI_NOZ, 0.0f};

			tr_r3d_emit_mesh(&dl, &cam, &l, &in);
		}
		bg.horizon = 0, bg.ground = 0xCB89; /* the glow, as RGB565 */
		render(&bg);
		/* fix round 8: TR_VIEW_H, not TR_R3D_H -- rows past it are the
		 * bottom video panel's now (r3d.h), never fog-filled, so scanning
		 * into them read a false step from real content to zero/garbage. */
		for (int y = TR_VIEW_H - 1; y >= 0; y--) {
			int v[3], d;

			ch8(px(360, y), v);
			d = abs(v[0] - fog[0]) + abs(v[1] - fog[1]) + abs(v[2] - fog[2]);
			if (prev >= 0) {
				dmax = abs(d - prev) > dmax ? abs(d - prev) : dmax;
				back = d - prev > back ? d - prev : back;
			}
			prev = d;
		}
		printf("ground fog: largest row step %d, largest step away from the fog %d\n", dmax, back);
		assert(dmax <= 24 && back <= 8);
		tr_r3d_tex[7] = NULL;
		tr_r3d_tex_fog[7] = (tr_tex_fog_t){0};
	}

	/* 6. The full sky (dither + stars + halo) is band-local: 40 bands, each
	 * into a band buffer with guard rows after it, never write past the band
	 * and give exactly the frame of one full-screen band. Several horizons, so
	 * stars fall on band-boundary rows. */
	{
		static uint16_t full[TR_R3D_W * TR_R3D_H], zfull[TR_R3D_W * TR_R3D_H];
		static uint16_t cg[(TR_BAND_H + 2) * TR_R3D_W], zg[(TR_BAND_H + 2) * TR_R3D_W];
		static const int32_t hz[] = {700, 717, 745, 1000};

		dl.n = 0;
		for (unsigned k = 0; k < sizeof(hz) / sizeof(hz[0]); k++) {
			tr_bg_t bg = {0};

			bg.horizon = hz[k], bg.top = 0x0806, bg.mid = 0x680C, bg.mid_q8 = 150, bg.bot = 0xCB89;
			bg.ground = 0xCB89, bg.fx = TR_BG_DITHER | TR_BG_STARS;
			bg.sun_x = 400, bg.sun_y = (int16_t)(hz[k] - 40), bg.halo_r = 200, bg.halo = 0xFD20;
			tr_raster_band(NULL, 0, 0, TR_R3D_H, zfull, full, &bg, &dl, setup, bins[0], 0);
			for (int b = 0; b < TR_BANDS; b++) {
				for (int i = TR_BAND_H * TR_R3D_W; i < (TR_BAND_H + 2) * TR_R3D_W; i++) {
					cg[i] = zg[i] = 0xA5A5;
				}
				tr_raster_band(fb, TR_R3D_W, b * TR_BAND_H, (b + 1) * TR_BAND_H, zg, cg, &bg, &dl, setup, bins[0], 0);
				for (int i = TR_BAND_H * TR_R3D_W; i < (TR_BAND_H + 2) * TR_R3D_W; i++) {
					assert(cg[i] == 0xA5A5 && zg[i] == 0xA5A5);
				}
			}
			/* TR_BANDS covers only the game viewport, not the whole
			 * 1280-row fb -- `full` is one unclipped call spanning all
			 * 1280 rows, `fb` only ever loops bands 0..TR_BANDS-1, so
			 * only THAT overlap is comparable (r3d.h TR_VIEW_H). */
			assert(memcmp(fb, full, (size_t)TR_BANDS * TR_BAND_H * TR_R3D_W * sizeof(fb[0])) == 0);
		}
		printf("sky: band-local, 40 bands == one full-screen band\n");
	}

	/* 5. Every zone's ground textures (P15, zones.h): indices within the
	 * fog palette; the scene binds all four slots fogged, the camera-side
	 * pair expanded to RGB565 (the plain level-0 path), the far pair
	 * index-only (tr_r3d_tex NULL: level 0 through the palette too). */
	{
		tr_scene_t s;
		static uint16_t tx[TR_TEX_DIM * TR_TEX_DIM];

		tr_scene_init(&s);
		for (int slot = 0; slot < 4; slot++) {
			assert((tr_r3d_tex[slot] != NULL) == (slot < 2) && tr_r3d_tex_fog[slot].idx != NULL &&
			       tr_r3d_tex_fog[slot].npal > 0);
		}
		/* The textures tile (P4): a feature cut by the tile edge -- a
		 * route or a pour that stops at column 127 or row 127 instead of
		 * wrapping -- draws a seam every repeat. Texels are split into
		 * background (the three weave shades of the mask: the three
		 * commonest colours) and features; across the wrap seam no more
		 * texels switch between the two than across three quarters of
		 * the interior edges. */
		static uint16_t hist[65536];

		for (int t = 0; t < 2 * TR_ZONES; t++) {
			const tr_ztex_t *zt     = &tr_ztex[t / 2][t & 1];
			uint16_t         bg3[3] = {0, 0, 0};

			assert(zt->n <= 16u); /* 4 bpp (zones.h) */
			for (uint32_t i = 0; i < TR_TEX_DIM * TR_TEX_DIM; i++) {
				assert(tr_ztex_at(zt, i) < zt->n);
				tx[i] = zt->pal[tr_ztex_at(zt, i)];
			}

			memset(hist, 0, sizeof(hist));
			for (int i = 0; i < TR_TEX_DIM * TR_TEX_DIM; i++) {
				hist[tx[i]]++;
			}
			for (int k = 0; k < 3; k++) {
				for (int c = 0; c < 65536; c++) {
					if (hist[c] > hist[bg3[k]] && (k < 1 || c != bg3[0]) && (k < 2 || c != bg3[1])) {
						bg3[k] = (uint16_t)c;
					}
				}
			}
			for (int axis = 0; axis < 2; axis++) {
				int br[TR_TEX_DIM], sorted[TR_TEX_DIM - 1];

				for (int a = 0; a < TR_TEX_DIM; a++) {
					br[a] = 0;
					for (int b = 0; b < TR_TEX_DIM; b++) {
						int      a1 = (a + 1) & (TR_TEX_DIM - 1);
						uint16_t c0 = tx[axis ? a * TR_TEX_DIM + b : b * TR_TEX_DIM + a];
						uint16_t c1 = tx[axis ? a1 * TR_TEX_DIM + b : b * TR_TEX_DIM + a1];
						int      f0 = c0 != bg3[0] && c0 != bg3[1] && c0 != bg3[2];
						int      f1 = c1 != bg3[0] && c1 != bg3[1] && c1 != bg3[2];

						br[a] += f0 != f1;
					}
				}
				for (int a = 0; a < TR_TEX_DIM - 1; a++) { /* insertion sort */
					int k = a;

					while (k > 0 && sorted[k - 1] > br[a]) {
						sorted[k] = sorted[k - 1];
						k--;
					}
					sorted[k] = br[a];
				}
				printf("zone %d texture %s %s seam: %d feature edges (interior p75 %d)\n", t / 2, (t & 1) ? "board" : "lane",
				       axis ? "row" : "column", br[TR_TEX_DIM - 1], sorted[(TR_TEX_DIM - 1) * 3 / 4]);
				assert(br[TR_TEX_DIM - 1] <= sorted[(TR_TEX_DIM - 1) * 3 / 4]);
			}
		}
		printf("scene textures fogged: %u + %u colours\n", (unsigned)tr_r3d_tex_fog[TR_SCENE_TEX_LANE].npal,
		       (unsigned)tr_r3d_tex_fog[TR_SCENE_TEX_BOARD].npal);
	}
	return 0;
}
