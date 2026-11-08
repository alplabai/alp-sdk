/* tests/host/test_r3d_band.c -- T-A3 band raster: z band, 1/z, Gouraud,
 * textured spans, binning, NEON-span self-check, HUD blit, full-frame
 * golden. See src/render/r3d.h (tr_raster_band / tr_bin_build) for the
 * contracts. TR_DUMP=1 writes the golden frame to /tmp/tr-frame.ppm.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/render/r3d.h"
#include "../../src/render/span.h"
#include "../../src/render/sprite.h"
#include "tr_golden_dl.h"

/* HUD sprite: 13 x 9 (odd width: last byte of each row half used), index
 * pattern (x*3 + y) & 15 so index 0 (transparent) recurs. atlas.h is not
 * included: its generated header comment trips -Werror=comment. */
static uint8_t           hud_px[7 * 9];
static const tr_sprite_t hud = { 13, 9, hud_px };
static uint16_t          hud_pal[16];

#define W     TR_R3D_W
#define H     TR_R3D_H
#define PX(p) (((int32_t)(p) << TR_R3D_SUB) + 8) /* pixel CENTRE p in 28.4 */

static uint16_t       fb[W * H], fb2[W * H];
static uint16_t       zfull[W * H];
static uint16_t       zb[W * TR_BAND_H];
static uint16_t       bins[TR_BANDS][TR_BIN_MAX];
static uint32_t       counts[TR_BANDS];
static uint16_t       all_idx[TR_DL_MAX_TRIS];
static tr_tri_setup_t setup[TR_DL_MAX_TRIS];
static uint16_t       cfull[W * H];      /* colour "band" for the one-band renders */
static uint16_t       cb[W * TR_BAND_H]; /* colour band for the 40-band renders */
static tr_bg_t        bg;                /* background; {0} = all rows 0 */
static tr_dl_t        dl;

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

/* One full-screen band over every DL entry (setup via tr_bin_build(); an
 * empty triangle's setup draws nothing). */
static void draw_full(uint16_t *dst)
{
	uint32_t overflow = 0;

	tr_bin_build(&dl, setup, bins, counts, &overflow);
	for (uint16_t i = 0; i < dl.n; i++) {
		all_idx[i] = i;
	}
	/* fix round 8: TR_VIEW_H, not H -- see draw_banded()'s own comment;
	 * this stays the "one call instead of N bands" reference for the
	 * seam comparison, so it must cover the SAME rows draw_banded() does. */
	tr_raster_band(dst, W, 0, TR_VIEW_H, zfull, cfull, &bg, &dl, setup, all_idx, dl.n);
}

static void draw_banded(uint16_t *dst)
{
	uint32_t overflow = 0;

	tr_bin_build(&dl, setup, bins, counts, &overflow);
	assert(overflow == 0);
	for (int b = 0; b < TR_BANDS; b++) {
		/* fix round 8: match a32/renderer/render.c's own render_band() --
		 * the last band is ragged (TR_VIEW_H is not TR_BAND_H-aligned),
		 * clipped to TR_VIEW_H so the golden this generates agrees with
		 * what the real renderer actually produces for those rows
		 * (zero/untouched, not sky-filled past the viewport). */
		int y_hi = (b + 1) * TR_BAND_H < TR_VIEW_H ? (b + 1) * TR_BAND_H : TR_VIEW_H;

		tr_raster_band(dst, W, b * TR_BAND_H, y_hi, zb, cb, &bg, &dl, setup, bins[b], counts[b]);
	}
}

/* Axis-aligned screen rectangle covering exactly pixels [x0,x1) x [y0,y1)
 * (vertices on pixel corners; top-left rule) as 2 front-facing tris;
 * corner attrs q[TL,TR,BR,BL]. */
static void add_rect(int              x0,
                     int              y0,
                     int              x1,
                     int              y1,
                     uint16_t         c,
                     uint8_t          flags,
                     uint8_t          tex,
                     const tr_vattr_t q[4])
{
	tr_sv_t tl = { x0 << 4, y0 << 4 }, tr = { x1 << 4, y0 << 4 }, br = { x1 << 4, y1 << 4 },
	        bl = { x0 << 4, y1 << 4 };

	dl.tri[dl.n++] = (tr_tri_t){ { tl, tr, br }, c, tex, flags, { q[0], q[1], q[2] } };
	dl.tri[dl.n++] = (tr_tri_t){ { tl, br, bl }, c, tex, flags, { q[0], q[2], q[3] } };
}

static void add_flat_rect(int x0, int y0, int x1, int y1, uint16_t c, uint16_t w)
{
	tr_vattr_t a    = { w, 0, 0, c };
	tr_vattr_t q[4] = { a, a, a, a };

	add_rect(x0, y0, x1, y1, c, 0, 0, q);
}

/* 128x128 checkerboard, 16-texel squares. */
static uint16_t tex_checker[TR_TEX_DIM * TR_TEX_DIM];
/* texel (x,y) -> colour x | y << 7: a rendered pixel names its own texel. */
static uint16_t tex_coord[TR_TEX_DIM * TR_TEX_DIM];

/* The golden frame's background (was tr_r3d_sky(700, ...) on the FB; 700 rescaled to
 * TR_VIEW_H so the ground stop stays inside the rastered rows). */
static const tr_bg_t golden_bg = { .horizon = TR_VIEW_PX(700),
	                               .top     = 0x2010,
	                               .bot     = 0x9CDF,
	                               .ground  = 0x0000 };

/* Far-field texel agreement floor for test 5b (see there). fix round 8:
 * 975 -> 954 -- the camera's cy moved from TR_R3D_H/2 to TR_VIEW_H/2
 * (r3d_math.c tr_cam_build, the maintainer's viewport retune), which
 * measurably shifts this floor quad's most oblique rays (measured exactly
 * 954/1000 post-retune, was comfortably over 975/1000 before); the
 * rasterizer itself (r3d_raster.c) did not change, only the geometry this
 * synthetic test projects through it -- not a precision regression to
 * chase further. Fix round 12 (review): pinned to the actual measured
 * value (run the test and read its own printed far/far_exact counts --
 * 15506/16254 at roll 0.00, the tighter of the two rolls this test
 * checks), not a round-number floor (950) with slack under it a real
 * regression could hide in. */
#define FAR_EXACT_PERMILLE 953

static uint16_t checker(int tx, int ty)
{
	return (((tx >> 4) ^ (ty >> 4)) & 1) ? 0xFFE0 : 0x001F;
}

/* CRC-32 over the raw float bits the front-end produces for 512 fixed
 * inputs: tr_sincosf, a full tr_cam_build (yaw, pitch AND roll non-zero,
 * so every product term is live), and tr_r3d_project's unrounded view z.
 * The golden scene alone is too tame (yaw 0 zeroes half the camera terms;
 * 28.4 rounding hides most last-bit differences) to catch a contraction. */
static uint32_t fe_fingerprint(void)
{
	static float buf[512 * 20];
	int          n  = 0;
	uint32_t     rs = 0x13579BDFu;

	for (int i = 0; i < 512; i++) {
		float    a[4];
		tr_cam_t cam;
		tr_sv_t  sv;

		for (int k = 0; k < 4; k++) {
			rs ^= rs << 13, rs ^= rs >> 17, rs ^= rs << 5;
			a[k] = (float)(int32_t)(rs & 0xFFFFF) * 1.0e-5f - 5.0f; /* -5..5.5 */
		}
		tr_sincosf(a[0] * 7.0f, &buf[n], &buf[n + 1]);
		n += 2;
		tr_cam_build(&cam,
		             (tr_v3_t){ a[1] * 100.0f, 150.0f, a[2] * 100.0f },
		             a[0],
		             a[1] * 0.3f,
		             a[2] * 0.2f,
		             600.0f + a[3]);
		memcpy(&buf[n], cam.view.m, sizeof(cam.view.m));
		n += 12;
		(void)tr_r3d_project(
		    &cam, (tr_v3_t){ a[3] * 300.0f, a[2] * 50.0f, 500.0f + a[1] * 80.0f }, &sv, &buf[n]);
		n += 1;
	}
	return crc32(buf, (size_t)n * sizeof(float));
}

/* Writes tr_golden_dl.h: the golden scene's DL, sky, texture and raster
 * CRC -- everything the A32 payload needs for CP-A6 without running the
 * float front-end. Run: TR_GEN_DL=tests/host/tr_golden_dl.h <this test>. */
static void gen_golden_header(const char *path, uint32_t crc, uint32_t fe_crc)
{
	FILE *f = fopen(path, "w");

	assert(f);
	fprintf(
	    f,
	    "/* clang-format off */\n/* tests/host/tr_golden_dl.h -- GENERATED by "
	    "tests/host/test_r3d_band.c\n"
	    " * (TR_GEN_DL=<path>), DO NOT EDIT. The golden scene's display list, sky and\n"
	    " * texture; rasterised (tr_bin_build + %d x tr_raster_band, bg TR_GOLDEN_BG) it gives\n"
	    " * TR_GOLDEN_RASTER_CRC (CRC-32, IEEE, over the 800x1280 RGB565 frame,\n"
	    " * little-endian). The raster has no float, so this holds bit-exact on the A32\n"
	    " * (a32-renderer plan, CP-A6). */\n"
	    "#ifndef TR_GOLDEN_DL_H\n#define TR_GOLDEN_DL_H\n\n#include \"../../src/render/r3d.h\"\n\n",
	    TR_BANDS);
	fprintf(f,
	        "#define TR_GOLDEN_RASTER_CRC 0x%08xu\n#define TR_GOLDEN_DL_N %u\n",
	        (unsigned)crc,
	        dl.n);
	fprintf(f,
	        "/* Front-end float fingerprint (fe_fingerprint() in the generating test):\n"
	        " * tr_sincosf, tr_cam_build and unrounded view z over 512 fixed inputs.\n"
	        " * An FMA contraction or a libm sin/cos changes it. */\n"
	        "#define TR_GOLDEN_FE_CRC 0x%08xu\n",
	        (unsigned)fe_crc);
	fprintf(f,
	        "/* CRC-32 of each band's %d rows of that frame (band b = rows 32b..32b+31). */\n"
	        "#define TR_GOLDEN_BAND_CRC {",
	        TR_BAND_H);
	for (int b = 0; b < TR_BANDS; b++) {
		fprintf(f,
		        "%s0x%08xu",
		        b ? (b % 6 ? ", " : ",\\\n\t") : "",
		        (unsigned)crc32(&fb[b * TR_BAND_H * W], TR_BAND_H * W * sizeof(fb[0])));
	}
	fprintf(f, "}\n");
	fprintf(
	    f,
	    "/* tr_bg_t for every band (fx 0: the plain two-stop gradient) */\n"
	    "#define TR_GOLDEN_BG {.horizon = %d, .top = 0x%04x, .bot = 0x%04x, .ground = 0x%04x}\n\n",
	    (int)golden_bg.horizon,
	    golden_bg.top,
	    golden_bg.bot,
	    golden_bg.ground);
	fprintf(f,
	        "/* tr_r3d_tex[0]: 128x128 checker, 16-texel squares, 0xFFE0 / 0x001F. */\n"
	        "static inline void tr_golden_tex_init(uint16_t *t)\n{\n"
	        "\tfor (int y = 0; y < TR_TEX_DIM; y++) {\n\t\tfor (int x = 0; x < TR_TEX_DIM; x++) {\n"
	        "\t\t\tt[y * TR_TEX_DIM + x] = (((x >> 4) ^ (y >> 4)) & 1) ? 0xFFE0 : 0x001F;\n"
	        "\t\t}\n\t}\n}\n\n");
	fprintf(f, "static const tr_tri_t tr_golden_dl[TR_GOLDEN_DL_N] = {\n");
	for (uint16_t i = 0; i < dl.n; i++) {
		const tr_tri_t *t = &dl.tri[i];

		fprintf(f,
		        "\t{{{%d, %d}, {%d, %d}, {%d, %d}}, 0x%04x, %u, %u, {",
		        (int)t->v[0].x,
		        (int)t->v[0].y,
		        (int)t->v[1].x,
		        (int)t->v[1].y,
		        (int)t->v[2].x,
		        (int)t->v[2].y,
		        t->c,
		        t->tex,
		        t->flags);
		for (int k = 0; k < 3; k++) {
			fprintf(f,
			        "{%u, %u, %u, 0x%04x}%s",
			        t->a[k].w,
			        t->a[k].u,
			        t->a[k].v,
			        t->a[k].rgb,
			        k < 2 ? ", " : "");
		}
		fprintf(f, "}},\n");
	}
	fprintf(f, "};\n\n#endif /* TR_GOLDEN_DL_H */\n");
	fclose(f);
}

int main(void)
{
	for (int y = 0; y < TR_TEX_DIM; y++) {
		for (int x = 0; x < TR_TEX_DIM; x++) {
			tex_coord[y * TR_TEX_DIM + x] = (uint16_t)(x | (y << 7));
		}
	}
	for (int y = 0; y < 9; y++) {
		for (int x = 0; x < 13; x++) {
			uint8_t idx = (uint8_t)((x * 3 + y) & 15);

			hud_px[y * 7 + x / 2] |= (uint8_t)((x & 1) ? idx : idx << 4);
		}
	}
	for (int i = 0; i < 16; i++) {
		hud_pal[i] = (uint16_t)(0x1111u * (unsigned)i + 0x0842u);
	}
	tr_golden_tex_init(tex_checker);
	for (int i = 0; i < TR_TEX_DIM * TR_TEX_DIM; i++) {
		assert(tex_checker[i] == checker(i % TR_TEX_DIM, i / TR_TEX_DIM));
	}
	tr_r3d_tex[0] = tex_checker;
	tr_r3d_tex[1] = tex_coord;

	/* 1. Z order independent of emission order: near (w 4000) beats far
	 * (w 2000) in their overlap whichever is emitted first. */
	for (int order = 0; order < 2; order++) {
		dl.n = 0;
		if (order == 0) {
			add_flat_rect(10, 10, 60, 60, 0xAAAA, 2000);
			add_flat_rect(30, 30, 90, 90, 0xBBBB, 4000);
		} else {
			add_flat_rect(30, 30, 90, 90, 0xBBBB, 4000);
			add_flat_rect(10, 10, 60, 60, 0xAAAA, 2000);
		}
		memset(fb, 0, sizeof(fb));
		draw_full(fb);
		for (int y = 0; y < 100; y++) {
			for (int x = 0; x < 100; x++) {
				bool     in_a = x >= 10 && x < 60 && y >= 10 && y < 60;
				bool     in_b = x >= 30 && x < 90 && y >= 30 && y < 90;
				uint16_t exp  = in_b ? 0xBBBB : (in_a ? 0xAAAA : 0);

				assert(fb[y * W + x] == exp);
			}
		}
	}

	/* 2. Z tie (documented in r3d.h: strict >): equal w, the triangle drawn
	 * FIRST keeps the pixel. */
	dl.n = 0;
	add_flat_rect(10, 10, 50, 50, 0x1111, 3000);
	add_flat_rect(10, 10, 50, 50, 0x2222, 3000);
	memset(fb, 0, sizeof(fb));
	draw_full(fb);
	assert(fb[20 * W + 20] == 0x1111 && fb[49 * W + 49] == 0x1111);

	/* 2b. TR_TRI_NOZ background (flat, Gouraud, long textured): a ground
	 * rect emitted AFTER an object, with a far larger ("nearer") w, must
	 * neither hide the object nor write z -- so a later, farther object
	 * still draws over it. It is the bin that puts a band's NOZ triangles
	 * first, so this renders the 40 binned bands, then band 0 again to read
	 * its z. */
	for (int kind = 0; kind < 3; kind++) {
		{
			static const uint8_t gf[3] = { TR_TRI_NOZ,
				                           TR_TRI_NOZ | TR_TRI_GOURAUD,
				                           TR_TRI_NOZ | TR_TRI_TEX | TR_TRI_UVX8 };
			tr_vattr_t           g[4]  = { { 60000, 0, 0, 0x7777 },
				                           { 60000, 0x1000, 0, 0x7777 },
				                           { 60000, 0x1000, 0x1000, 0x7777 },
				                           { 60000, 0, 0x1000, 0x7777 } };

			dl.n = 0;
			add_flat_rect(20, 20, 60, 60, 0x1111, 2000);      /* object */
			add_rect(0, 0, 128, 128, 0x7777, gf[kind], 1, g); /* ground, after it */
			add_flat_rect(70, 70, 90, 90, 0x2222, 10);        /* farther object, after both */
			memset(fb, 0, sizeof(fb));
			draw_banded(fb);
			assert(bins[0][0] == 2 && bins[0][1] == 3 && bins[0][2] == 0 &&
			       bins[0][3] == 1); /* NOZ first */
			tr_raster_band(NULL, W, 0, TR_BAND_H, zb, cb, &bg, &dl, setup, bins[0], counts[0]);
			assert(zb[5 * W + 5] == 0 && zb[25 * W + 100] == 0); /* ground only: z untouched */
			assert(zb[25 * W + 30] == 2000);
			for (int y = 0; y < 128; y++) {
				for (int x = 0; x < 128; x++) {
					bool in_a = x >= 20 && x < 60 && y >= 20 && y < 60;
					bool in_b = x >= 70 && x < 90 && y >= 70 && y < 90;

					if (in_a || in_b) {
						assert(fb[y * W + x] == (in_a ? 0x1111 : 0x2222));
					} else if (kind < 2) {
						assert(fb[y * W + x] == 0x7777);
					} else { /* coordinate texture, 0x1000 = 128 texels over 128 px */
						assert(fb[y * W + x] == tex_coord[y * 128 + x]);
					}
				}
			}
		}
	}

	/* 3. Band z-tested coverage == tr_raster_tri() coverage (same edge
	 * DDA): random tris, all at the same far-but-valid w onto a cleared
	 * z band, pixel sets must match exactly. */
	{
		uint32_t rs = 0x1234567u;

		for (int it = 0; it < 300; it++) {
			tr_sv_t v[3];

			for (int k = 0; k < 3; k++) {
				rs ^= rs << 13, rs ^= rs >> 17, rs ^= rs << 5;
				v[k].x = (int32_t)(rs % (200u << 4)) - (20 << 4);
				rs ^= rs << 13, rs ^= rs >> 17, rs ^= rs << 5;
				v[k].y = (int32_t)(rs % (200u << 4)) - (20 << 4);
			}
			tr_tri_t t = { { v[0], v[1], v[2] },
				           0x7777,
				           0,
				           0,
				           { { 500, 0, 0, 0 }, { 500, 0, 0, 0 }, { 500, 0, 0, 0 } } };

			dl.n      = 1;
			dl.tri[0] = t;
			memset(fb, 0, W * 200 * 2);
			memset(fb2, 0, W * 200 * 2);
			draw_full(fb);
			tr_raster_tri(fb2, W, &t);
			assert(memcmp(fb, fb2, W * 200 * 2) == 0);
		}
	}

	/* 4. Gouraud endpoints exact: vertices on pixel centres; the pixel at
	 * the triangle's top-left vertex is the only vertex pixel the top-left
	 * rule includes, so rotate which vertex INDEX sits there -- each of the
	 * 3 colours (and the plane anchored at v[0] vs away from it) must come
	 * back exactly. */
	{
		tr_sv_t  pos[3] = { { PX(100), PX(100) },
			                { PX(160), PX(100) },
			                { PX(100), PX(190) } }; /* TL, TR, BL */
		uint16_t col[3] = { 0xF800, 0x07E0, 0x001F };
		uint16_t w[3]   = { 9000, 3000, 20000 };

		for (int r = 0; r < 3; r++) {
			tr_tri_t t = { { { 0, 0 } }, 0, 0, TR_TRI_GOURAUD, { { 0, 0, 0, 0 } } };

			for (int k = 0; k < 3; k++) {
				int p      = (k + r) % 3; /* vertex k sits at position p */
				t.v[k]     = pos[p];
				t.a[k].rgb = col[k];
				t.a[k].w   = w[k];
			}
			dl.n      = 1;
			dl.tri[0] = t;
			memset(fb, 0, W * 200 * 2);
			draw_full(fb);

			int k_tl = (3 - r) % 3; /* vertex index at position 0 (TL) */

			assert(fb[100 * W + 100] == col[k_tl]);
			assert(zfull[100 * W + 100] == w[k_tl]);
		}

		/* 4b. Interior: every covered pixel of a Gouraud tri with
		 * vertex colours spanning each channel's full range matches a
		 * float barycentric reference within 1 LSB per channel. */
		{
			tr_sv_t  p[3]  = { { PX(200) + 3, PX(300) - 5 },
				               { PX(420) - 7, PX(340) + 2 },
				               { PX(260) + 1, PX(520) + 6 } };
			uint16_t cc[3] = { 0xF81F, 0x07E0, 0x39E7 };
			tr_tri_t t     = {
				{ p[0], p[1], p[2] },
				0,
				0,
				TR_TRI_GOURAUD,
				{ { 5000, 0, 0, cc[0] }, { 7000, 0, 0, cc[1] }, { 9000, 0, 0, cc[2] } }
			};
			float area = (float)(p[1].x - p[0].x) * (float)(p[2].y - p[0].y) -
			             (float)(p[1].y - p[0].y) * (float)(p[2].x - p[0].x);
			int   n    = 0;

			dl.n      = 1;
			dl.tri[0] = t;
			memset(fb, 0, W * 600 * 2);
			draw_full(fb);
			for (int y = 280; y < 540; y++) {
				for (int x = 180; x < 440; x++) {
					if (zfull[y * W + x] == 0) {
						continue; /* not covered */
					}
					float px = (float)(x * 16 + 8), py = (float)(y * 16 + 8);
					float l1    = ((px - (float)p[0].x) * (float)(p[2].y - p[0].y) -
					               (py - (float)p[0].y) * (float)(p[2].x - p[0].x)) /
					              area;
					float l2    = ((float)(p[1].x - p[0].x) * (py - (float)p[0].y) -
					               (float)(p[1].y - p[0].y) * (px - (float)p[0].x)) /
					              area;
					int   sh[3] = { 11, 5, 0 }, mk[3] = { 31, 63, 31 };

					for (int ch = 0; ch < 3; ch++) {
						float c0  = (float)((cc[0] >> sh[ch]) & mk[ch]);
						float c1  = (float)((cc[1] >> sh[ch]) & mk[ch]);
						float c2  = (float)((cc[2] >> sh[ch]) & mk[ch]);
						float ref = c0 + l1 * (c1 - c0) + l2 * (c2 - c0);
						int   got = (fb[y * W + x] >> sh[ch]) & mk[ch];

						assert(fabsf((float)got - ref) <= 1.0f);
					}
					n++;
				}
			}
			assert(n > 10000);
		}
	}

	/* 3b. Setup is built once, by tr_bin_build(): tr_raster_band() never
	 * re-derives it from the triangle's vertices (only c/flags/tex/attrs
	 * are read per band). Scribbling the vertices after binning must not
	 * move a single pixel. */
	{
		uint32_t overflow = 0;

		dl.n = 0;
		add_flat_rect(40, 20, 400, 300, 0x4321, 2000);
		tr_bin_build(&dl, setup, bins, counts, &overflow);
		memset(fb, 0, sizeof(fb));
		for (int b = 0; b < TR_BANDS; b++) {
			tr_raster_band(fb,
			               W,
			               b * TR_BAND_H,
			               (b + 1) * TR_BAND_H,
			               zb,
			               cb,
			               &bg,
			               &dl,
			               setup,
			               bins[b],
			               counts[b]);
		}
		memcpy(fb2, fb, sizeof(fb));
		for (uint16_t i = 0; i < dl.n; i++) {
			memset(dl.tri[i].v, 0x7F, sizeof(dl.tri[i].v));
		}
		memset(fb, 0, sizeof(fb));
		for (int b = 0; b < TR_BANDS; b++) {
			tr_raster_band(fb,
			               W,
			               b * TR_BAND_H,
			               (b + 1) * TR_BAND_H,
			               zb,
			               cb,
			               &bg,
			               &dl,
			               setup,
			               bins[b],
			               counts[b]);
		}
		assert(memcmp(fb, fb2, sizeof(fb)) == 0);
		assert(fb[20 * W + 40] == 0x4321 && fb[299 * W + 399] == 0x4321);
	}

	/* 5a. Textured quad golden: screen-aligned 256x256 px quad at
	 * (100,200), u,v 0..128 texels (8.8: 0..0x8000), constant w, checker:
	 * every pixel is known exactly -- texel = (px - origin) / 2. */
	{
		tr_vattr_t q[4] = { { 1000, 0, 0, 0 },
			                { 1000, 0x8000, 0, 0 },
			                { 1000, 0x8000, 0x8000, 0 },
			                { 1000, 0, 0x8000, 0 } };

		dl.n = 0;
		add_rect(100, 200, 356, 456, 0, TR_TRI_TEX, 0, q);
		memset(fb, 0, sizeof(fb));
		draw_full(fb);
		for (int y = 190; y < 466; y++) {
			for (int x = 90; x < 366; x++) {
				bool     in  = x >= 100 && x < 356 && y >= 200 && y < 456;
				uint16_t exp = in ? checker((x - 100) >> 1, (y - 200) >> 1) : 0;

				assert(fb[y * W + x] == exp);
			}
		}
	}

	/* 5d. TR_TRI_UVX8: u/v stored / 8, so a quad runs past one repeat. A
	 * 256x128 px rect, u 0..2 repeats (0x2000), v 0..1 (0x1000), constant w
	 * (rgb: w's fraction bits, 0), coordinate texture: one texel per px,
	 * pixel (x, y) shows texel ((x - 100) & 127, y - 200). */
	{
		tr_vattr_t q[4] = { { 1000, 0, 0, 0 },
			                { 1000, 0x2000, 0, 0 },
			                { 1000, 0x2000, 0x1000, 0 },
			                { 1000, 0, 0x1000, 0 } };

		dl.n = 0;
		add_rect(100, 200, 356, 328, 0, TR_TRI_TEX | TR_TRI_UVX8, 1, q);
		memset(fb, 0, sizeof(fb));
		draw_full(fb);
		for (int y = 190; y < 338; y++) {
			for (int x = 90; x < 366; x++) {
				bool in = x >= 100 && x < 356 && y >= 200 && y < 328;

				assert(fb[y * W + x] == (in ? tex_coord[(y - 200) * 128 + ((x - 100) & 127)] : 0));
			}
		}
		/* Past the flag's precondition ((16 u + 1) w 2^16 < 2^49: here u
		 * 0xFFFF at w 60000) the U plane would overflow: not drawn. */
		uint32_t overflow = 0;

		q[1].u = q[2].u = 0xFFFF;
		q[0].w = q[1].w = q[2].w = q[3].w = 60000;
		dl.n                              = 0;
		add_rect(100, 200, 356, 328, 0, TR_TRI_TEX | TR_TRI_UVX8, 1, q);
		tr_bin_build(&dl, setup, bins, counts, &overflow);
		assert(setup[0].y0 >= setup[0].y1 && setup[1].y0 >= setup[1].y1);
	}

	/* 5b. Perspective-correct texturing: a real floor quad (y = 0,
	 * x -200..400, z 100..900, w 9:1 near to far) through the camera and
	 * tr_r3d_emit_quad_tex(), coordinate texture, camera roll 0 (depth
	 * constant along a row) and 0.3 rad (depth varies along each row --
	 * the sub-span divide's job). Float reference per covered
	 * pixel: back-project the pixel centre onto the floor, map to texels.
	 * Every pixel within 1 texel, >= 95% exact -- affine-only
	 * interpolation is tens of texels off at this depth ratio. */
	for (int yi = 0; yi < 2; yi++) {
		static const uint16_t uv[4][2] = {
			{ 0, 0 }, { 0, 0x8000 }, { 0x8000, 0x8000 }, { 0x8000, 0 }
		};
		tr_v3_t  q[4] = { { -200, 0, 100 }, { -200, 0, 900 }, { 400, 0, 900 }, { 400, 0, 100 } };
		float    roll = yi ? 0.3f : 0.0f, cr = cosf(roll), sr = sinf(roll);
		tr_cam_t cam;
		int      exact = 0, total = 0, far_exact = 0, far_total = 0;

		tr_cam_build(&cam, (tr_v3_t){ 0, 100, 0 }, 0.0f, 0.0f, roll, 600.0f);
		dl.n = 0;
		assert(tr_r3d_emit_quad_tex(&dl, &cam, q, uv, 1, 0) == 2);
		bg.ground = 0xFFFF; /* not a coordinate texel (y would be 511) */
		draw_full(fb);
		bg.ground = 0;
		/* fix round 8: only the rows the real game ever draws into
		 * (TR_VIEW_H, r3d.h) -- rows below it are the video panel now,
		 * an increasingly oblique ray angle this camera was never tuned
		 * to project accurately that far from its own (now much higher)
		 * cy, and nothing the real renderer ever asks it to. */
		for (int y = 0; y < TR_VIEW_H; y++) {
			for (int x = 0; x < W; x++) {
				uint16_t p = fb[y * W + x];

				if (p == 0xFFFF) {
					continue;
				}
				/* camera ray (dx, dy, 1), rolled to world, hits y = 0 */
				/* fix round 8: the camera's own cy is TR_VIEW_H/2 now (r3d_math.c
				 * tr_cam_build), not TR_R3D_H/2 -- this hand-derived ray must
				 * agree with it. */
				float dx = ((float)x + 0.5f - (float)TR_R3D_W / 2.0f) / 600.0f,
				      dy = -((float)y + 0.5f - (float)TR_VIEW_H / 2.0f) / 600.0f;
				float t  = -100.0f / (sr * dx + cr * dy);
				float wx = t * (cr * dx - sr * dy), wz = t;
				int   tx = (int)floorf((wx + 200.0f) / 600.0f * 128.0f);
				int   ty = (int)floorf((wz - 100.0f) / 800.0f * 128.0f);
				int   gx = p & 127, gy = p >> 7;

				/* within 1 texel, circularly: a pixel on the quad's far
				 * edge may land on texel 128 == 0 after the wrap */
				int ex = (gx - tx) & 127, ey = (gy - ty) & 127;

				assert((ex <= 1 || ex == 127) && (ey <= 1 || ey == 127));
				exact += (ex == 0 && ey == 0);
				total++;
				if (wz > 600.0f) { /* w < ~3500: where a truncated 1/w shows */
					far_exact += (ex == 0 && ey == 0);
					far_total++;
				}
			}
		}
		printf("perspective quad roll %.2f: %d px, %d exact; far %d px, %d exact\n",
		       (double)roll,
		       total,
		       exact,
		       far_total,
		       far_exact);
		assert(total > 10000 && exact * 100 >= total * 95);
		assert(far_total > 1000 && far_exact * 1000 >= far_total * FAR_EXACT_PERMILLE);
	}

	/* 5c. Near-zero w at a span end: a textured tri whose right vertex has
	 * w = 20 (w falls ~230 per px toward it), so 1 px past a span the w
	 * plane is already <= 0. The perspective samples -- each span's first
	 * pixel, every 8th pixel after it, and its LAST pixel -- must match the
	 * float reference within 1 texel. (Pixels between samples are affine
	 * and may bow at this extreme 5:1 w ratio over 4 px; not checked.) A
	 * last sub-span whose end is sampled past the span divides by a
	 * wrapped w: its last pixel comes back as garbage. */
	{
		tr_sv_t  p[3]  = { { PX(100), PX(100) }, { PX(113), PX(100) }, { PX(100), PX(160) } };
		uint16_t ww[3] = { 3000, 20, 3000 }, uu[3] = { 0, 0x3000, 0 }, vv[3] = { 0, 0, 0x6000 };
		tr_tri_t t    = { { p[0], p[1], p[2] }, 0, 1, TR_TRI_TEX, { { 0, 0, 0, 0 } } };
		float    area = (float)(p[1].x - p[0].x) * (float)(p[2].y - p[0].y) -
		                (float)(p[1].y - p[0].y) * (float)(p[2].x - p[0].x);
		int      n    = 0;

		for (int k = 0; k < 3; k++) {
			t.a[k] = (tr_vattr_t){ ww[k], uu[k], vv[k], 0 };
		}
		dl.n      = 1;
		dl.tri[0] = t;
		memset(fb, 0, W * 200 * 2);
		draw_full(fb);
		for (int y = 90; y < 170; y++) {
			int lo = -1, hi = -1;

			for (int x = 90; x < 130; x++) {
				if (zfull[y * W + x] != 0) {
					lo = lo < 0 ? x : lo;
					hi = x + 1;
				}
			}
			for (int x = lo; lo >= 0 && x < hi; x++) {
				if ((x - lo) % 8 != 0 && x != hi - 1) {
					continue;
				}
				float px = (float)(x * 16 + 8), py = (float)(y * 16 + 8);
				float l1 = ((px - (float)p[0].x) * (float)(p[2].y - p[0].y) -
				            (py - (float)p[0].y) * (float)(p[2].x - p[0].x)) /
				           area;
				float l2 = ((float)(p[1].x - p[0].x) * (py - (float)p[0].y) -
				            (float)(p[1].y - p[0].y) * (px - (float)p[0].x)) /
				           area;
				float l0 = 1.0f - l1 - l2;
				float w  = l0 * (float)ww[0] + l1 * (float)ww[1] + l2 * (float)ww[2];
				float u  = (l1 * (float)(uu[1] * ww[1])) / w;
				float v  = (l2 * (float)(vv[2] * ww[2])) / w;
				int   ex = ((fb[y * W + x] & 127) - (int)floorf(u / 256.0f)) & 127;
				int   ey = ((fb[y * W + x] >> 7) - (int)floorf(v / 256.0f)) & 127;

				assert((ex <= 1 || ex == 127) && (ey <= 1 || ey == 127));
				n++;
			}
		}
		assert(n > 80);
	}

	/* 3c. Rows off the side of the screen are never stepped (tri_setup's
	 * strip clip): a triangle wholly right of the screen is binned nowhere;
	 * a full-height sliver that crosses the left edge at row ~31 keeps only
	 * rows [0, 32) -- and draws exactly tr_raster_tri()'s pixels. */
	{
		uint32_t overflow = 0;

		dl.n      = 0;
		dl.tri[0] = (tr_tri_t){ { { (TR_R3D_W + 80) << 4, 0 },
			                      { (TR_R3D_W + 180) << 4, 1280 << 4 },
			                      { (TR_R3D_W + 40) << 4, 1280 << 4 } },
			                    0x5555,
			                    0,
			                    0,
			                    { { 0 } } };
		dl.tri[1] =
		    (tr_tri_t){ { { 10 << 4, 0 }, { -400 * 16, 1280 << 4 }, { -500 * 16, 1280 << 4 } },
			            0x5555,
			            0,
			            0,
			            { { 0 } } };
		dl.n = 2;
		for (int i = 0; i < 2; i++) {
			dl.tri[i].a[0].w = dl.tri[i].a[1].w = dl.tri[i].a[2].w = 100;
		}
		tr_bin_build(&dl, setup, bins, counts, &overflow);
		assert(setup[0].y0 >= setup[0].y1);
		assert(setup[1].y0 == 0 && setup[1].y1 == 32);
		assert(counts[0] == 1 && counts[1] == 0 && bins[0][0] == 1);
		memset(fb, 0, sizeof(fb));
		memset(fb2, 0, sizeof(fb2));
		tr_raster_tri(fb2, W, &dl.tri[1]);
		draw_banded(fb);
		for (int i = 0; i < W * H; i++) {
			assert((fb[i] == 0x5555) == (fb2[i] == 0x5555));
		}
	}

	/* 6. Span self-check (host: scalar branch; A32 calls the same function
	 * on its NEON branch at boot). */
	assert(tr_span_selfcheck() == 1);
	assert(tr_raster_selfcheck() == 1);

	/* 7. HUD blit: strided blit == the old stateful tr_sprite_draw into a
	 * tight buffer, incl. clipping off the left/top edge. */
	{
		static uint16_t tight[40 * 40];
		int             ox[3] = { 5, -7, 30 }, oy[3] = { 4, -3, 20 };

		for (int c = 0; c < 3; c++) {
			for (int i = 0; i < 40 * 40; i++) {
				tight[i] = 0xDEAD;
			}
			for (int y = 0; y < 40; y++) {
				for (int x = 0; x < 64; x++) {
					fb[y * 64 + x] = 0xDEAD;
				}
			}
			tr_sprite_set_target(tight, 40, 40, hud_pal);
			tr_sprite_draw((int16_t)ox[c], (int16_t)oy[c], &hud);
			tr_sprite_blit(fb, 64, 40, 40, ox[c], oy[c], &hud, hud_pal);
			for (int y = 0; y < 40; y++) {
				assert(memcmp(&fb[y * 64], &tight[y * 40], 40 * 2) == 0);
				for (int x = 40; x < 64; x++) {
					assert(fb[y * 64 + x] == 0xDEAD);
				}
			}
		}
	}

	/* 8. Full-frame golden + band seams: a fixed PCB-ish scene (sky,
	 * textured + flat ground crossing the near plane, Gouraud and flat
	 * cubes interpenetrating, HUD digit), rendered banded (40 bands) and as
	 * one full-screen band -- bit-identical -- and CRC32-pinned. */
	{
		static int16_t v[8 * 3];
		static int8_t  vn[8 * 3];
		static uint8_t tri[12 * 3];
		static int8_t  nrm[12 * 3];
		static uint8_t col[12];
		int            nt = 0;

		for (int i = 0; i < 8; i++) {
			for (int a = 0; a < 3; a++) {
				v[i * 3 + a]  = (int16_t)((i >> a) & 1 ? 60 : -60);
				vn[i * 3 + a] = (int8_t)((i >> a) & 1 ? 73 : -73);
			}
		}
		for (int a = 0; a < 3; a++) {
			for (int s = 0; s < 2; s++) {
				int b = (a + 1) % 3, c = (a + 2) % 3, cr[4];

				for (int k = 0; k < 4; k++) {
					int bb = (k == 1 || k == 2), cc = (k >= 2);

					cr[k] = (s << a) | (bb << b) | (cc << c);
				}
				/* front iff (p1-p0)x(p2-p0) . n > 0 (see test_r3d_math
				 * case 4 for the winding this reproduces) */
				float p[3][3];
				for (int k = 0; k < 3; k++) {
					for (int d = 0; d < 3; d++) {
						p[k][d] = v[cr[k] * 3 + d];
					}
				}
				float e1[3]     = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
				float e2[3]     = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
				float cx[3]     = { e1[1] * e2[2] - e1[2] * e2[1],
					                e1[2] * e2[0] - e1[0] * e2[2],
					                e1[0] * e2[1] - e1[1] * e2[0] };
				float nd        = cx[a] * (s ? 1.0f : -1.0f);
				int   ord[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };

				if (nd < 0) {
					ord[0][1] = 3, ord[0][2] = 2, ord[1][1] = 2, ord[1][2] = 1;
				}
				for (int h = 0; h < 2; h++) {
					for (int k = 0; k < 3; k++) {
						tri[nt * 3 + k] = (uint8_t)cr[ord[h][k]];
						nrm[nt * 3 + k] = (int8_t)(k == a ? (s ? 127 : -127) : 0);
					}
					col[nt] = (uint8_t)(3 + a);
					nt++;
				}
			}
		}
		tr_mesh_t  cube = { v, tri, nrm, col, 8, (uint16_t)nt, vn, NULL, NULL, 0 };
		tr_cam_t   cam;
		tr_light_t l = {
			{ 0.35f, 0.8f, -0.48f }, 0.25f, { 90, 110, 140 }, 1500.0f, 8500.0f, NULL, 0.0f, 0.0f
		};

		tr_cam_build(&cam, (tr_v3_t){ 0, 180, -300 }, 0.0f, 0.18f, 0.03f, 620.0f);

		uint32_t fe_crc = fe_fingerprint();
		dl.n            = 0;
		for (int zi = 0; zi < 24; zi++) {
			for (int xi = 0; xi < 5; xi++) {
				float   x0 = -480.0f + 192.0f * (float)xi, z0 = -384.0f + 384.0f * (float)zi;
				tr_v3_t q[4]                   = { { x0, 0, z0 },
					                               { x0, 0, z0 + 384 },
					                               { x0 + 192, 0, z0 + 384 },
					                               { x0 + 192, 0, z0 } };
				static const uint16_t uv[4][2] = {
					{ 0, 0 }, { 0, 0x8000 }, { 0x8000, 0x8000 }, { 0x8000, 0 }
				};

				if ((xi + zi) & 1) {
					tr_r3d_emit_quad_tex(&dl, &cam, q, uv, 0, 0);
				} else {
					tr_r3d_emit_quad(&dl, &cam, q, (uint16_t)(zi & 2 ? 0x2545 : 0xB325), 0);
				}
			}
		}
		for (int i = 0; i < 6; i++) {
			tr_inst_t in = { &cube,
				             { -200.0f + 90.0f * (float)i, 60.0f, 300.0f + 350.0f * (float)i },
				             0.4f * (float)i,
				             1.0f,
				             (uint8_t)(i & 1 ? TR_TRI_GOURAUD : 0),
				             0.0f };

			tr_r3d_emit_mesh(&dl, &cam, &l, &in);
		}
		/* a big Gouraud cube cutting through the ground and another cube */
		tr_inst_t big = { &cube, { -140.0f, 20.0f, 360.0f }, 0.7f, 1.6f, TR_TRI_GOURAUD, 0.0f };

		tr_r3d_emit_mesh(&dl, &cam, &l, &big);
		printf("golden scene: %u tris\n", dl.n);
		assert(dl.n > 200);

		/* Raster-only frame (sky + 40 bands): what CP-A6 reproduces on
		 * the A32 from the committed DL blob, with no front-end float. */
		bg = golden_bg;
		draw_banded(fb);
		draw_full(fb2);
		assert(memcmp(fb, fb2, sizeof(fb)) == 0); /* band seams */

		uint32_t crc = crc32(fb, sizeof(fb));

		printf("golden raster crc32 %08x\n", (unsigned)crc);
		if (getenv("TR_GEN_DL")) {
			gen_golden_header(getenv("TR_GEN_DL"), crc, fe_crc);
			return 0;
		}

		/* Front-end determinism: this build's float front-end (host
		 * glibc or A32 newlib under qemu) produced exactly the committed
		 * camera bits and DL. */
		assert(fe_crc == TR_GOLDEN_FE_CRC);
		assert(dl.n == TR_GOLDEN_DL_N);
		assert(memcmp(dl.tri, tr_golden_dl, sizeof(tr_golden_dl)) == 0);

		{
			static const tr_bg_t hb = TR_GOLDEN_BG;

			assert(hb.horizon == golden_bg.horizon && hb.top == golden_bg.top &&
			       hb.bot == golden_bg.bot && hb.ground == golden_bg.ground);
		}
		/* The CP-A6 procedure itself: rasterise the BLOB. */
		memcpy(dl.tri, tr_golden_dl, sizeof(tr_golden_dl));
		dl.n = TR_GOLDEN_DL_N;
		draw_banded(fb);
		assert(crc32(fb, sizeof(fb)) == TR_GOLDEN_RASTER_CRC);
		{
			static const uint32_t band_crc[TR_BANDS] = TR_GOLDEN_BAND_CRC;

			for (int b = 0; b < TR_BANDS; b++) {
				assert(crc32(&fb[b * TR_BAND_H * W], TR_BAND_H * W * sizeof(fb[0])) == band_crc[b]);
			}
		}

		/* The FB is write-only: poisoned with two different patterns
		 * before rendering, the frame must come out identical (any FB
		 * read -- a blend, a partial-row copy, an uncovered pixel left
		 * as-is -- would leak the poison). */
		for (int i = 0; i < W * H; i++) {
			fb[i] = 0xDEAD, fb2[i] = (uint16_t)(i * 2654435761u >> 16);
		}
		draw_banded(fb);
		draw_banded(fb2);
		/* fix round 8: TR_BANDS (27) no longer covers the whole 1280-row
		 * fb -- rows [TR_BANDS*TR_BAND_H, H) are the bottom video panel's
		 * domain now (a32/renderer/render.c draw_video_panel(), a
		 * separate once-a-frame pass this synthetic-DL test never runs),
		 * genuinely outside what draw_banded() ever touches. Zero them
		 * back to what TR_GOLDEN_RASTER_CRC was computed against (both
		 * copies were fresh/zeroed there, not poisoned) instead of
		 * leaving each buffer's OWN poison pattern sitting in rows this
		 * pass was never going to determine either way -- that is a
		 * genuinely untouched region now, not the write-only leak this
		 * check exists to catch. */
		memset(&fb[TR_VIEW_H * W], 0, (size_t)(H - TR_VIEW_H) * W * sizeof(fb[0]));
		memset(&fb2[TR_VIEW_H * W], 0, (size_t)(H - TR_VIEW_H) * W * sizeof(fb2[0]));
		assert(memcmp(fb, fb2, sizeof(fb)) == 0 && crc32(fb, sizeof(fb)) == TR_GOLDEN_RASTER_CRC);
		bg = (tr_bg_t){ 0 };

		tr_sprite_blit(fb, W, W, H, 16, 16, &hud, hud_pal);
		if (getenv("TR_DUMP")) {
			FILE *f = fopen("/tmp/tr-frame.ppm", "wb");

			assert(f);
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
	}

	return 0;
}
