/* tests/host/test_r3d_raster.c -- tr_raster_tri()/tr_r3d_draw() (the
 * fixed-point rasterizer) and tr_span_fill()'s scalar path (span.h). See
 * src/render/r3d.h for the contracts under test.
 *
 * tr_raster_tri()'s screen clip is against the FIXED TR_R3D_W x TR_R3D_H
 * (720x1280) panel size, not a caller-supplied window -- so testing the
 * real screen-edge clip needs a buffer whose logical (0,0) really is
 * screen (0,0). This file allocates ONE canvas sized to the real panel
 * plus a canary margin on all 4 sides and reuses it for every case; a
 * write outside [0,720)x[0,1280) shows up as a stomped canary byte, not
 * just a wrong on-screen pixel.
 */
/* tr_tri_t grew per-vertex attributes (T-A3); this file's positional
 * {{v0, v1, v2}, colour} initialisers deliberately leave them zero -- the
 * cases below test coverage only, unchanged. */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../../src/render/r3d.h"
#include "../../src/render/span.h"

/*
 * The rasterizer's OLD algorithm, kept here as a test-only reference: a
 * per-pixel edge test, exactly what tr_raster_tri() itself did before
 * fix-round-1's review (O(bbox area) was ~138 ms/frame; fix-round-1's
 * exact-per-row solve was itself ~18 ms/frame of __aeabi_ldivmod calls;
 * the real rasterizer is now the incremental DDA in r3d_raster.c). Moved
 * out of r3d_raster.c by fix-round-2's review, point 3 -- this is
 * test-only code and has no reason to sit in target .text.
 *
 * A standalone copy of edge()/is_top_left(), not a call into
 * r3d_raster.c's (those are `static`, and rightly so -- nothing outside
 * that file should depend on the real rasterizer's internal helpers).
 * The formulas themselves are exactly r3d.h's documented winding/top-left
 * convention (see that header's top comment), so this is checking
 * r3d_raster.c against the SPEC, not against a second copy of its
 * implementation.
 */
static int64_t ref_edge(tr_sv_t a, tr_sv_t b, tr_sv_t p)
{
	return (int64_t)(b.x - a.x) * (int64_t)(p.y - a.y) -
	       (int64_t)(b.y - a.y) * (int64_t)(p.x - a.x);
}

static bool ref_is_top_left(tr_sv_t a, tr_sv_t b)
{
	int32_t dx = b.x - a.x;
	int32_t dy = b.y - a.y;

	return (dy == 0 && dx > 0) || (dy < 0);
}

static bool tr_raster_point_inside_ref(tr_sv_t a, tr_sv_t b, tr_sv_t c, tr_sv_t p)
{
	int64_t e0 = ref_edge(a, b, p);
	int64_t e1 = ref_edge(b, c, p);
	int64_t e2 = ref_edge(c, a, p);

	if (!(e0 > 0 || (e0 == 0 && ref_is_top_left(a, b)))) {
		return false;
	}
	if (!(e1 > 0 || (e1 == 0 && ref_is_top_left(b, c)))) {
		return false;
	}
	if (!(e2 > 0 || (e2 == 0 && ref_is_top_left(c, a)))) {
		return false;
	}
	return true;
}

#define SENTINEL 0xDEADu /* Never a real fill colour: proves "untouched". */
#define PX(px)   ((int32_t)(px) * (1 << TR_R3D_SUB)) /* multiply: px may be negative */

/* xorshift32, deterministic fixed seed -- reproducible test runs, not a
 * real PRNG's job (just needs to cover a wide, varied spread of triangles
 * every run, not to be unpredictable). */
static uint32_t rng_state = 0xA53Cu ^ 0x1234567u;

static uint32_t rng_next(void)
{
	uint32_t x = rng_state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;
	return x;
}

/* Random int32 in [lo, hi) -- hi > lo required, range comfortably under
 * UINT32_MAX so the modulo bias is negligible for a test generator. */
static int32_t rng_range(int32_t lo, int32_t hi)
{
	return lo + (int32_t)(rng_next() % (uint32_t)(hi - lo));
}

#define CANARY_W 4
#define PHYS_W   (TR_R3D_W + 2 * CANARY_W)
#define PHYS_H   (TR_R3D_H + 2 * CANARY_W)

static uint16_t phys[PHYS_W * PHYS_H];

static void fb_reset(void)
{
	for (size_t i = 0; i < (size_t)PHYS_W * PHYS_H; i++) {
		phys[i] = SENTINEL;
	}
}

/* Pointer to logical screen (0,0) -- what tr_raster_tri()'s `fb` argument
 * and PHYS_W as `stride_px` together address exactly like the real panel,
 * with CANARY_W rows/cols of untouchable margin on every side. */
static uint16_t *fb_origin(void)
{
	return &phys[(size_t)CANARY_W * PHYS_W + CANARY_W];
}

static uint16_t at(int32_t x, int32_t y)
{
	return phys[(size_t)(y + CANARY_W) * PHYS_W + (size_t)(x + CANARY_W)];
}

/* Mutable reference to the same cell -- section 5's per-iteration window
 * reset writes through this instead of a full fb_reset() (see there). */
static uint16_t *at_mut(int32_t x, int32_t y)
{
	return &phys[(size_t)(y + CANARY_W) * PHYS_W + (size_t)(x + CANARY_W)];
}

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
	if (v < lo) {
		return lo;
	}
	if (v > hi) {
		return hi;
	}
	return v;
}

/* One full on-screen-width row of SENTINEL, filled once in main() below --
 * the memcmp() reference for "this whole row/margin is untouched",
 * section 1+2's fast path for the area outside a shape's local window. */
static uint16_t sentinel_row[TR_R3D_W];

static void assert_canary_intact(void)
{
	for (int32_t y = -CANARY_W; y < TR_R3D_H + CANARY_W; y++) {
		for (int32_t x = -CANARY_W; x < TR_R3D_W + CANARY_W; x++) {
			if (x >= 0 && x < TR_R3D_W && y >= 0 && y < TR_R3D_H) {
				continue;
			}
			assert(at(x, y) == SENTINEL);
		}
	}
}

int main(void)
{
	for (int32_t i = 0; i < TR_R3D_W; i++) {
		sentinel_row[i] = SENTINEL;
	}

	/*
	 * 1+2. Quad coverage (top-left rule: no cracks, no overdraw) +
	 * sub-pixel offsets + clipping at all four real screen edges,
	 * combined: an axis-aligned square split by its TL-BR diagonal into
	 * 2 triangles (winding chosen front-facing -- positive screen area,
	 * same convention pinned down in test_r3d_math.c), rasterized into
	 * TWO SEPARATE draws of the SAME canvas so each triangle's own
	 * footprint is directly observable and directly compared against the
	 * OTHER triangle's footprint for the "exactly once" property.
	 *
	 * off (1/16 px, TR_R3D_SUB units) sub-pixel-shifts the square so its
	 * edges don't land on whole-pixel boundaries; 8 (half a pixel, an
	 * exact tie) is deliberately excluded -- this test re-derives the
	 * expected inside/outside set from float geometry (`inside` above),
	 * independent of how the rasterizer resolves ties, so it is
	 * deliberately never asked to also get an exact tie right by hand.
	 *
	 * corner sweeps the square across the real panel: comfortably
	 * on-screen, and straddling each of the left/right/top/bottom edges
	 * (720/1280) -- draw_and_check_square_half()'s per-pixel scan over
	 * the WHOLE panel (not just a local window) is what proves the
	 * clipped-away portion writes nothing, on all 4 edges.
	 */
	{
		/*
		 * 8 (== half a pixel) is included deliberately, not excluded: it
		 * is the one offset where the square's OUTER edges land exactly
		 * on pixel centres -- an exact top-left-rule tie on a boundary
		 * only one triangle owns (the top/left/right/bottom edges of the
		 * square, as opposed to the shared internal diagonal). Every
		 * other offset here (0, 3, 6, 11, 14) never produces a pixel
		 * centre exactly on ANY edge (outer or diagonal) of this square,
		 * so without offset 8 the `inside` reference below would never
		 * actually depend on whether >= or > is used at the boundary --
		 * a flipped top-left rule would silently pass. See this task's
		 * report for the red run this offset was added to make fail.
		 */
		static const int32_t offs[]     = { 0, 3, 6, 8, 11, 14 };
		static const int32_t corner_x[] = { 300, -6, TR_R3D_W - 10, 300, 300 };
		/* fix round 8 moved this to TR_VIEW_H - 40 (clear of tri_setup()'s
		 * then-unconditional TR_VIEW_H clip, load-bearing for the BINNED
		 * path) rather than fix the real gap: tr_raster_tri() (this
		 * file's own subject, called with tri_setup's `planes=false`)
		 * never touches BINS at all and its own documented contract here
		 * is the FULL TR_R3D_H screen. Fix round 12 (review) made that
		 * clip `planes`-conditional (r3d_raster.c) -- tr_raster_tri()
		 * genuinely clips to TR_R3D_H again now, so the true bottom-edge
		 * case is restored, not still an open follow-up. */
		static const int32_t corner_y[] = { 300, 300, 300, -6, TR_R3D_H - 10 };
		int32_t              side       = 16;

		for (size_t oi = 0; oi < sizeof(offs) / sizeof(offs[0]); oi++) {
			for (size_t ci = 0; ci < 5; ci++) {
				int32_t cxf   = PX(corner_x[ci]) + offs[oi];
				int32_t cyf   = PX(corner_y[ci]) + offs[oi];
				int32_t sidef = PX(side);

				tr_sv_t tl = { cxf, cyf };
				tr_sv_t tr = { cxf + sidef, cyf };
				tr_sv_t br = { cxf + sidef, cyf + sidef };
				tr_sv_t bl = { cxf, cyf + sidef };

				double ox = (double)cxf / (double)(1 << TR_R3D_SUB);
				double oy = (double)cyf / (double)(1 << TR_R3D_SUB);

				/* Draw each triangle alone first, to check ITS OWN
				 * footprint never exceeds the square (a triangle that
				 * over-draws past the diagonal would be caught here
				 * even before the union/overlap check below). */
				fb_reset();
				{
					tr_tri_t t1 = { { tl, tr, br }, 0x1111 };

					tr_raster_tri(fb_origin(), PHYS_W, &t1);
				}
				assert_canary_intact();

				/* Snapshot t1's on-screen footprint (which pixels it
				 * touched, ignoring colour), then redraw t2 alone into
				 * a clean canvas and compare footprints directly. */
				static uint16_t snap1[PHYS_W * PHYS_H];

				memcpy(snap1, phys, sizeof(phys));

				fb_reset();
				{
					tr_tri_t t2 = { { tl, br, bl }, 0x2222 };

					tr_raster_tri(fb_origin(), PHYS_W, &t2);
				}
				assert_canary_intact();

				int32_t wx0 = (int32_t)ox - 4, wx1 = (int32_t)(ox + (double)side) + 5;
				int32_t wy0 = (int32_t)oy - 4, wy1 = (int32_t)(oy + (double)side) + 5;

				/*
				 * Screen-clipped window bounds: everything OUTSIDE this
				 * window is checked with one memcmp() per row/margin
				 * against an all-SENTINEL reference row instead of a
				 * per-pixel loop -- this test's own runtime, not the
				 * rasterizer's, was the thing scanning the full
				 * 720x1280 panel 30 times over; the actual per-pixel
				 * top-left-rule check below still runs on every pixel
				 * that could possibly be inside the square.
				 */
				int32_t cwx0 = clampi(wx0, 0, TR_R3D_W), cwx1 = clampi(wx1, 0, TR_R3D_W);
				int32_t cwy0 = clampi(wy0, 0, TR_R3D_H), cwy1 = clampi(wy1, 0, TR_R3D_H);

				for (int32_t y = 0; y < TR_R3D_H; y++) {
					size_t row_idx = (size_t)(y + CANARY_W) * PHYS_W + CANARY_W;

					if (y < cwy0 || y >= cwy1) {
						assert(memcmp(&snap1[row_idx], sentinel_row, sizeof(sentinel_row)) == 0);
						assert(memcmp(&phys[row_idx], sentinel_row, sizeof(sentinel_row)) == 0);
						continue;
					}
					if (cwx0 > 0) {
						assert(memcmp(&snap1[row_idx], sentinel_row, (size_t)cwx0 * 2) == 0);
						assert(memcmp(&phys[row_idx], sentinel_row, (size_t)cwx0 * 2) == 0);
					}
					if (cwx1 < TR_R3D_W) {
						size_t rem = (size_t)(TR_R3D_W - cwx1);

						assert(memcmp(&snap1[row_idx + (size_t)cwx1], sentinel_row, rem * 2) == 0);
						assert(memcmp(&phys[row_idx + (size_t)cwx1], sentinel_row, rem * 2) == 0);
					}

					for (int32_t x = cwx0; x < cwx1; x++) {
						size_t idx        = row_idx + (size_t)x;
						bool   t1_touched = snap1[idx] != SENTINEL;
						bool   t2_touched = phys[idx] != SENTINEL;

						/*
						 * Half-open, top/left INCLUSIVE and
						 * bottom/right EXCLUSIVE (>= low, < high on
						 * both axes) -- the exact inclusion rule an
						 * axis-aligned box gets under the top-left
						 * fill rule (top and left edges are "in",
						 * right and bottom are not), independent of
						 * how the box happens to be split into
						 * triangles. At offset 8 (see offs[]'s
						 * comment) this is the difference that
						 * actually matters; at every other offset in
						 * this sweep no pixel centre lands exactly on
						 * a boundary, so >= / < and > / < agree.
						 */
						double px_c   = (double)x + 0.5;
						double py_c   = (double)y + 0.5;
						bool   inside = px_c >= ox && px_c < ox + (double)side && py_c >= oy &&
						                py_c < oy + (double)side;

						if (inside) {
							assert(t1_touched != t2_touched); /* exactly once */
							if (t1_touched) {
								assert(snap1[idx] == 0x1111);
							} else {
								assert(phys[idx] == 0x2222);
							}
						} else {
							assert(!t1_touched && !t2_touched); /* never */
						}
					}
				}
			}
		}
	}

	/* 3. Zero-area triangles write nothing: 2 coincident vertices, and 3
	 * exactly-collinear vertices (both give a screen-space area of
	 * exactly 0, which the area <= 0 backface/degenerate test rejects --
	 * see r3d.h's top comment). */
	{
		tr_sv_t a            = { PX(5), PX(5) };
		tr_sv_t b            = { PX(10), PX(5) };
		tr_sv_t c_coincident = a;
		tr_sv_t c_collinear  = { PX(15), PX(5) }; /* same row as a, b: collinear */

		tr_tri_t t_coincident = { { a, b, c_coincident }, 0x3333 };
		tr_tri_t t_collinear  = { { a, b, c_collinear }, 0x3333 };

		fb_reset();
		tr_raster_tri(fb_origin(), PHYS_W, &t_coincident);
		assert_canary_intact();
		for (int32_t y = 0; y < 20; y++) {
			for (int32_t x = 0; x < 20; x++) {
				assert(at(x, y) == SENTINEL);
			}
		}

		fb_reset();
		tr_raster_tri(fb_origin(), PHYS_W, &t_collinear);
		assert_canary_intact();
		for (int32_t y = 0; y < 20; y++) {
			for (int32_t x = 0; x < 20; x++) {
				assert(at(x, y) == SENTINEL);
			}
		}
	}

	/* 3b. Fully off-screen triangles (well past TR_R3D_W/TR_R3D_H in
	 * every direction -- the clamped bounding box is empty) write
	 * nothing at all. */
	{
		int32_t far = PX(100000);

		struct {
			tr_sv_t v[3];
		} cases[4] = {
			{ { { -far, PX(5) }, { -far + PX(2), PX(6) }, { -far, PX(7) } } }, /* far left */
			{ { { far, PX(5) }, { far + PX(2), PX(6) }, { far, PX(7) } } },    /* far right */
			{ { { PX(5), -far }, { PX(6), -far + PX(2) }, { PX(7), -far } } }, /* far above */
			{ { { PX(5), far }, { PX(6), far + PX(2) }, { PX(7), far } } },    /* far below */
		};

		for (int i = 0; i < 4; i++) {
			/* Reorder so each is front-facing (area > 0) -- picked by
			 * trial against tr_raster_tri()'s own edge() sign, same
			 * convention as every other case in this file; irrelevant to
			 * the result here (an off-screen backface would also write
			 * nothing), but keeping every case in this file front-facing
			 * means "writes nothing" is always because of the SCREEN
			 * clip, not silently because of the AREA cull. */
			tr_tri_t t = { { cases[i].v[0], cases[i].v[2], cases[i].v[1] }, 0x4444 };

			fb_reset();
			tr_raster_tri(fb_origin(), PHYS_W, &t);
			assert_canary_intact();
			for (int32_t y = 0; y < 20; y++) {
				for (int32_t x = 0; x < 20; x++) {
					assert(at(x, y) == SENTINEL);
				}
			}
		}
	}

	/*
	 * 4. Scalar tr_span_fill() == a hand-written reference loop, for
	 * every span length 0..40 AND at every alignment 0..8 (odd
	 * included) -- this is what a broken tail predicate/remainder-loop
	 * boundary looks like on host, where __ARM_FEATURE_MVE is never
	 * defined so this always exercises the #else scalar path in span.h.
	 */
	{
		uint16_t buf[64];
		uint16_t ref[64];

		for (uint32_t start = 0; start < 9; start++) {
			for (uint32_t n = 0; n <= 40; n++) {
				if (start + n > 64) {
					continue;
				}
				for (int i = 0; i < 64; i++) {
					buf[i] = SENTINEL;
					ref[i] = SENTINEL;
				}
				tr_span_fill(&buf[start], n, 0x5555);
				for (uint32_t i = 0; i < n; i++) {
					ref[start + i] = 0x5555;
				}
				assert(memcmp(buf, ref, sizeof(buf)) == 0);
			}
		}
	}

	/*
	 * 5. Differential test: the fast scanline tr_raster_tri() must
	 * produce EXACTLY the pixel set tr_raster_point_inside_ref() (the old,
	 * proven-correct per-pixel implementation -- section 1+2/3/3b above
	 * were all written and green against it) would, on thousands of
	 * random triangles -- fix-round-1 review's own prescribed oracle for
	 * "must remain bit-identical". Compared over a local window
	 * [0, WIN) x [0, WIN), comfortably inside the real screen so no
	 * triangle here is screen-clip-limited by construction (screen-edge
	 * clipping is already covered by section 1+2's dedicated corner
	 * cases) -- this section's whole job is the INTERIOR top-left-rule
	 * arithmetic, not re-proving the edge clip.
	 *
	 * Most triangle vertices are local (small, sub-pixel-varied, inside
	 * or just outside the window -- exercises real boundaries); 1 in 4
	 * triangles gets one vertex replaced by a guard-band-scale coordinate
	 * (up to +-2,000,000 in 28.4, i.e. up to +-125,000 px) specifically
	 * to exercise edge_row_bound()'s int64 arithmetic at that scale, per
	 * the review's "incl. guard-band-sized ones".
	 */
	{
#define WIN 48
		/*
		 * Reset the FULL canvas once, not once per iteration (that was
		 * ~938K writes x 5000 iterations -- this test's own reset cost,
		 * not the rasterizer's, was what made the differential test
		 * slow). Each iteration below resets only its own WIN x WIN
		 * window before drawing into it; a large or guard-band-scale
		 * triangle may still paint pixels outside that window (harmless
		 * -- nothing outside the window is ever compared against
		 * `expected`), and assert_canary_intact() runs once, after every
		 * iteration, catching any OOB write from ANY of the 5000
		 * triangles (a canary cell, once stomped, never gets un-stomped
		 * by a later legitimate on-screen write).
		 */
		fb_reset();
		for (int iter = 0; iter < 5000; iter++) {
			tr_sv_t v[3];

			for (int i = 0; i < 3; i++) {
				v[i].x = rng_range(-8, WIN + 8) * (1 << TR_R3D_SUB) + rng_range(0, 1 << TR_R3D_SUB);
				v[i].y = rng_range(-8, WIN + 8) * (1 << TR_R3D_SUB) + rng_range(0, 1 << TR_R3D_SUB);
			}
			if (iter % 4 == 0) {
				int vi   = rng_range(0, 3);
				int sign = (rng_range(0, 2) == 0) ? 1 : -1;

				v[vi].x = sign * rng_range(50000, 2000000);
				v[vi].y = sign * rng_range(50000, 2000000);
			}

			/* Flip winding (swap v[1]/v[2]) if backface -- an
			 * always-empty triangle (both sides agree trivially) tells
			 * this test nothing about the top-left-rule arithmetic;
			 * bias toward the interesting, non-degenerate case. Uses
			 * the SAME edge() formula as r3d_raster.c/r3d_math.c,
			 * reimplemented locally (not calling either file's static
			 * function) so this generator has no dependency on their
			 * internals. */
			int64_t area = (int64_t)(v[1].x - v[0].x) * (int64_t)(v[2].y - v[0].y) -
			               (int64_t)(v[1].y - v[0].y) * (int64_t)(v[2].x - v[0].x);

			if (area <= 0) {
				tr_sv_t tmp = v[1];

				v[1] = v[2];
				v[2] = tmp;
			}

			bool expected[WIN][WIN];

			for (int32_t y = 0; y < WIN; y++) {
				for (int32_t x = 0; x < WIN; x++) {
					tr_sv_t p = { PX(x) + (1 << (TR_R3D_SUB - 1)),
						          PX(y) + (1 << (TR_R3D_SUB - 1)) };

					expected[y][x] = tr_raster_point_inside_ref(v[0], v[1], v[2], p);
				}
			}

			for (int32_t y = 0; y < WIN; y++) {
				for (int32_t x = 0; x < WIN; x++) {
					*at_mut(x, y) = SENTINEL;
				}
			}

			tr_tri_t tri = { { v[0], v[1], v[2] }, 0x7777 };

			tr_raster_tri(fb_origin(), PHYS_W, &tri);

			for (int32_t y = 0; y < WIN; y++) {
				for (int32_t x = 0; x < WIN; x++) {
					bool got = at(x, y) != SENTINEL;

					assert(got == expected[y][x]);
					if (got) {
						assert(at(x, y) == 0x7777);
					}
				}
			}
		}
		assert_canary_intact(); /* once, over all 5000 iterations -- see above */
#undef WIN
	}

	/*
	 * 6. tr_r3d_draw(): painter's order -- a later triangle in the
	 * display list overwrites an earlier one in their overlap, drawn in
	 * dl->tri[] index order (no z-test, no sort -- r3d.h's own contract).
	 * Two overlapping axis-aligned squares (each 2 triangles, same
	 * front-facing winding convention as every other case in this file):
	 * square A [10,30)x[10,30) colour 0xAAAA drawn first, square B
	 * [20,40)x[20,40) colour 0xBBBB drawn second, overlapping in
	 * [20,30)x[20,30).
	 */
	{
		tr_dl_t dl = { 0 };

		tr_sv_t a_tl = { PX(10), PX(10) }, a_tr = { PX(30), PX(10) };
		tr_sv_t a_br = { PX(30), PX(30) }, a_bl = { PX(10), PX(30) };
		tr_sv_t b_tl = { PX(20), PX(20) }, b_tr = { PX(40), PX(20) };
		tr_sv_t b_br = { PX(40), PX(40) }, b_bl = { PX(20), PX(40) };

		dl.tri[dl.n++] = (tr_tri_t){ { a_tl, a_tr, a_br }, 0xAAAA };
		dl.tri[dl.n++] = (tr_tri_t){ { a_tl, a_br, a_bl }, 0xAAAA };
		dl.tri[dl.n++] = (tr_tri_t){ { b_tl, b_tr, b_br }, 0xBBBB };
		dl.tri[dl.n++] = (tr_tri_t){ { b_tl, b_br, b_bl }, 0xBBBB };

		fb_reset();
		tr_r3d_draw(fb_origin(), PHYS_W, &dl);
		assert_canary_intact();

		for (int32_t y = 0; y < 45; y++) {
			for (int32_t x = 0; x < 45; x++) {
				bool     in_a = x >= 10 && x < 30 && y >= 10 && y < 30;
				bool     in_b = x >= 20 && x < 40 && y >= 20 && y < 40;
				uint16_t got  = at(x, y);

				if (in_b) {
					assert(got == 0xBBBB); /* B drawn last -- wins the overlap */
				} else if (in_a) {
					assert(got == 0xAAAA);
				} else {
					assert(got == SENTINEL);
				}
			}
		}
	}

	/*
	 * 7. tr_r3d_sky(): horizon_y <= 0 (nothing drawn), == 1 (single row,
	 * exactly `top`), a mid-panel horizon (row 0 exactly `top`, last row
	 * within +-1/32 (in 8-bit-per-channel terms) of `bot` -- lerp565()'s
	 * own 5/6/5<->8-bit widen/narrow round trip, checked independently
	 * here via float rather than by re-deriving its exact integer
	 * formula), and horizon_y > TR_R3D_H (clamps to exactly TR_R3D_H rows,
	 * not more). Every row checked for being a FLAT fill (every x on a
	 * row identical), not just its endpoints.
	 */
	{
		uint16_t top = 0x2010, bot = 0xF81F; /* dark blue-green -> magenta */

		/* horizon_y <= 0: nothing drawn at all. */
		fb_reset();
		tr_r3d_sky(fb_origin(), PHYS_W, 0, top, bot);
		assert_canary_intact();
		for (int32_t y = 0; y < 5; y++) {
			for (int32_t x = 0; x < 5; x++) {
				assert(at(x, y) == SENTINEL);
			}
		}
		fb_reset();
		tr_r3d_sky(fb_origin(), PHYS_W, -5, top, bot);
		for (int32_t x = 0; x < 5; x++) {
			assert(at(x, 0) == SENTINEL);
		}

		/* horizon_y == 1: exactly row 0, coloured exactly `top` (t == 0
		 * at the only row), row 1 untouched. */
		fb_reset();
		tr_r3d_sky(fb_origin(), PHYS_W, 1, top, bot);
		for (int32_t x = 0; x < TR_R3D_W; x++) {
			assert(at(x, 0) == top);
		}
		assert(at(0, 1) == SENTINEL);

		/* Mid-panel horizon. */
		{
			int32_t horizon = 384;

			fb_reset();
			tr_r3d_sky(fb_origin(), PHYS_W, horizon, top, bot);
			assert_canary_intact();

			for (int32_t x = 0; x < TR_R3D_W; x += 37) { /* flat-row spot check */
				assert(at(x, 0) == top);
			}
			assert(at(0, horizon) == SENTINEL); /* first row NOT drawn */

			/* Independent float reference for the last row (t == 1.0
			 * exactly): widen 5/6/5 -> 8-bit the same way lerp565()
			 * does, lerp at t=1 (== bot exactly, no rounding possible),
			 * narrow back -- computed here from first principles, not
			 * by importing lerp565()'s own formula. */
			int      br = (bot >> 11) & 0x1F, bg = (bot >> 5) & 0x3F, bb = bot & 0x1F;
			int      br8 = (br << 3) | (br >> 2), bg8 = (bg << 2) | (bg >> 4),
			         bb8 = (bb << 3) | (bb >> 2);
			uint16_t expect_last =
			    (uint16_t)(((br8 & 0xF8) << 8) | ((bg8 & 0xFC) << 3) | (bb8 >> 3));

			for (int32_t x = 0; x < TR_R3D_W; x += 37) {
				assert(at(x, horizon - 1) == expect_last);
			}
			assert(at(0, horizon) == SENTINEL); /* one past the last drawn row */
		}

		/* horizon_y > TR_R3D_H: clamps to exactly TR_R3D_H rows -- the
		 * last real row is drawn, row TR_R3D_H itself (out of the panel)
		 * is not. */
		{
			fb_reset();
			tr_r3d_sky(fb_origin(), PHYS_W, TR_R3D_H + 50, top, bot);
			assert_canary_intact(); /* proves row TR_R3D_H was never touched */
			for (int32_t x = 0; x < TR_R3D_W; x += 37) {
				assert(at(x, 0) == top);
				assert(at(x, TR_R3D_H - 1) != SENTINEL);
			}
		}
	}

	/*
	 * 8. Cycle smoke (no hard threshold -- printed for visibility before
	 * silicon, per fix-round-1 review): a ~1,000-tri "frame" tiling the
	 * whole TR_R3D_W x TR_R3D_H panel (24 x 20 grid of quads, 2 tris
	 * each = 960 tris, ~921,600 px, close to the plan's ~1.1M px/frame
	 * estimate with its ~25% overdraw), timed with clock() over several
	 * iterations to average out scheduler noise.
	 */
	{
		enum { GRID_COLS = 24, GRID_ROWS = 20, ITERS = 20 };
		int32_t        cell_w = TR_R3D_W / GRID_COLS; /* 30, exact */
		int32_t        cell_h = TR_R3D_H / GRID_ROWS; /* 64, exact */
		static tr_dl_t dl;

		dl.n = 0;
		for (int32_t gy = 0; gy < GRID_ROWS; gy++) {
			for (int32_t gx = 0; gx < GRID_COLS; gx++) {
				int32_t  x0 = PX(gx * cell_w), x1 = PX((gx + 1) * cell_w);
				int32_t  y0 = PX(gy * cell_h), y1 = PX((gy + 1) * cell_h);
				tr_sv_t  tl = { x0, y0 }, tr = { x1, y0 }, br = { x1, y1 }, bl = { x0, y1 };
				uint16_t colour = (uint16_t)(0x1111 * ((gx + gy) % 8 + 1));

				dl.tri[dl.n++] = (tr_tri_t){ { tl, tr, br }, colour };
				dl.tri[dl.n++] = (tr_tri_t){ { tl, br, bl }, colour };
			}
		}

		clock_t t0 = clock();

		for (int i = 0; i < ITERS; i++) {
			tr_r3d_draw(fb_origin(), PHYS_W, &dl);
		}
		clock_t t1 = clock();

		double  sec          = (double)(t1 - t0) / (double)CLOCKS_PER_SEC;
		double  ns_per_frame = (sec / ITERS) * 1.0e9;
		int32_t px_per_frame = TR_R3D_W * TR_R3D_H;

		printf("cycle smoke: %u tris, %d px/frame, %.0f ns/frame (host, %d iters)\n",
		       dl.n,
		       (int)px_per_frame,
		       ns_per_frame,
		       ITERS);
	}

	return 0;
}
