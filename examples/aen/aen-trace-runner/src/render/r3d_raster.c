/* src/render/r3d_raster.c -- sky gradient and the fixed-point rasterizer
 * (tr_raster_tri()/tr_r3d_draw()). See r3d.h for the contracts and
 * coordinate conventions, and the real-3D plan (docs/superpowers/plans/
 * 2026-09-22-real-3d-renderer.md, section 5, T4) for the task this is.
 *
 * Integer 28.4 throughout, no float in any value (recip62()'s double is only
 * an estimate, corrected to the exact integer): bit-exact between host and target
 * (plan section 2, "Numeric"), which is what makes a host-built golden
 * frame meaningful. Deliberately free of Zephyr/alp-sdk headers, same
 * convention as proj.c, so tests/host/runner.sh compiles this straight
 * into every host test.
 */
#include <stddef.h>
#include <string.h>

#include "r3d.h"
#include "span.h"

/* TR_RASTER_CHECKS (tests/host/runner.sh sets it for every host and A32 qemu
 * build; never the renderer image): the invariants a single-threaded test
 * cannot see by output -- e.g. a masked NEON chunk reading and rewriting past
 * its row, harmless alone, but on core 0's last colour-band row a race with
 * core 1's z band (a32/renderer/render.c ZBAND/CBAND). */
#ifdef TR_RASTER_CHECKS
#include <assert.h>
#define RASTER_CHECK(c) assert(c)
#else
#define RASTER_CHECK(c) ((void)0)
#endif

#define SUB_ONE  (1 << TR_R3D_SUB) /* 1.0 px in 28.4 */
#define SUB_HALF (SUB_ONE >> 1)    /* 0.5 px in 28.4 -- pixel-centre sampling */

/*
 * Screen-space edge function from a->b evaluated at p, same formula and
 * same sign convention as r3d_math.c's signed_area() (front-facing total
 * area, and every one of its 3 edge terms individually, is POSITIVE --
 * see r3d.h's top comment). int64: coordinates run up to the guard band
 * (+-16384 px in 28.4 == +-262144), whose product can reach ~6.9e10,
 * well past int32.
 */
static int64_t edge(tr_sv_t a, tr_sv_t b, tr_sv_t p)
{
	return (int64_t)(b.x - a.x) * (int64_t)(p.y - a.y) -
	       (int64_t)(b.y - a.y) * (int64_t)(p.x - a.x);
}

/*
 * Top-left fill rule bias for edge a->b, in a CLOCKWISE-on-screen (y down)
 * front-facing winding (r3d.h's top comment): a "top" edge is horizontal
 * and runs left-to-right (dy == 0, dx > 0); a "left" edge runs upward
 * (dy < 0) -- every other edge is a "right/bottom" edge. A point exactly on
 * a top or left edge (edge value == 0) is INSIDE; a point exactly on a
 * right or bottom edge is OUTSIDE. This is what makes two triangles that
 * share an edge (e.g. a quad's diagonal split) fill every pixel on that
 * edge exactly once between them, with neither a 1-px crack nor a 1-px
 * double-draw -- the two triangles see the SAME shared edge in opposite
 * directions (b->a instead of a->b), so exactly one of the two calls
 * classifies it top-or-left.
 */
static bool is_top_left(tr_sv_t a, tr_sv_t b)
{
	int32_t dx = b.x - a.x;
	int32_t dy = b.y - a.y;

	return (dy == 0 && dx > 0) || (dy < 0);
}

/*
 * The old per-pixel reference test (tr_raster_point_inside_ref()) used to
 * live here -- fix-round-2 review moved it to tests/host/test_r3d_raster.c
 * (it has no callers left in this file or in r3d.h; keeping test-only code
 * with external linkage in the target-compiled source was exactly what
 * put it in r3d_raster.o's .text for no reason). See that test file's own
 * copy for the differential-test oracle.
 */

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

/* floor(a / b), b > 0, correct for negative `a` (plain C `/` truncates
 * toward zero, which is NOT floor for negative a -- e.g. -1/16 truncates
 * to 0, but floor(-1/16) is -1). Every per-row boundary below is an exact
 * integer floor or ceiling of a rational edge-crossing, never a
 * float/double, so a guard-band-scale numerator (~6.9e10, same bound as
 * edge()) still resolves to the exact integer pixel column -- there is no
 * rounding error left to accumulate row-to-row. */
static int64_t floor_div(int64_t a, int64_t b)
{
	/* b > 0 always (a 16*|dy| denominator). On-screen triangles have
	 * int32-sized numerators: one hardware SDIV instead of a
	 * __aeabi_ldivmod call (the per-triangle setup's main cost on the A32).
	 * Same quotient either way. */
	if (a == (int32_t)a && b == (int32_t)b) {
		int32_t q32 = (int32_t)a / (int32_t)b, r32 = (int32_t)a % (int32_t)b;

		return r32 < 0 ? q32 - 1 : q32;
	}
	int64_t q = a / b;
	int64_t r = a % b;

	return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

/*
 * Incremental per-row bound for ONE non-horizontal edge -- an exact
 * rational DDA (Bresenham-style quotient + remainder), replacing the
 * exact-per-row SOLVE this file used to do (fix-round-1 review's fix):
 * that version called floor_div()/ceil_div() -- i.e. __aeabi_ldivmod --
 * once per non-horizontal edge PER ROW, ~144k times/frame on target
 * (fix-round-2 review, BLOCKER 1: ~18 ms at 400 MHz, still 4.7x the 3.8 ms
 * raster sub-budget). This struct's `quot`/`rem` are exactly that solve's
 * result for the CURRENT row, kept incrementally: ONE division at setup
 * (edge_dda_init_upper()/_lower() below, called once per edge per
 * triangle, not per row) computes the initial quot/rem AND a fixed
 * per-row step (step_quot/step_rem); edge_dda_step() advances to the next
 * row with pure int32 add/compare -- no division, no int64, anywhere in
 * the per-row path.
 *
 * `quot`/`rem`/`step_quot`/`step_rem`/`denom` all fit comfortably in
 * int32: quot is a pixel column (bounded by the guard band, +-16384),
 * denom is 16*|dy| (dy bounded by the guard band too, so denom <=
 * ~4.2M), and rem/step_rem are always in [0, denom) by construction
 * (floor division's remainder is never negative for a positive divisor).
 * Only the ONE-TIME setup division (in the two init functions) needs
 * int64 -- K and the un-shifted numerator can be guard-band-scale
 * products (~6.9e10), the same bound as edge()'s.
 */
typedef tr_edge_dda_t edge_dda_t; /* r3d.h: lives in tr_tri_setup_t */

/*
 * The per-row step times TR_BAND_H, as the same exact quotient/remainder
 * pair: 32 single steps == one band step, bit for bit, so a band entry
 * (edge_dda_skip()) is int32 adds. 32 * step_rem < 32 * denom <= 2^28 fits
 * int32; the divide is 32-bit (UDIV/SDIV), setup only.
 */
static void edge_dda_band_step(edge_dda_t *e)
{
	int32_t t = e->step_rem * TR_BAND_H;

	e->q32 = e->step_quot * TR_BAND_H + t / e->denom;
	e->r32 = t % e->denom;
}

/*
 * dy > 0 edge: `quot` is px_max, the largest px with px_c < K/dy (see
 * edge_dda_t's comment for the inequality this solves and why -- this is
 * the exact same "e > 0 strictly" test tr_raster_point_inside_ref() (now
 * in the test file) does per pixel, solved once here); a row's exclusive
 * upper bound is quot + 1. `pcy0` is the pixel-centre y of the FIRST row
 * this triangle draws (tr_raster_tri()'s y0) -- every subsequent row is
 * reached by edge_dda_step(), never by calling this again.
 */
static void edge_dda_init_upper(edge_dda_t *e, tr_sv_t a, tr_sv_t b, int64_t dy, int32_t pcy0)
{
	int64_t dx    = (int64_t)(b.x - a.x);
	int64_t K     = dx * (int64_t)(pcy0 - a.y) + dy * (int64_t)a.x;
	int64_t denom = 16 * dy;
	int64_t numer = K - 8 * dy - 1;
	int64_t q     = floor_div(numer, denom);
	int64_t r     = numer - q * denom;
	/* K (and so numer) increases by dx*16 for every +1 row (pcy advances
	 * by SUB_ONE == 16 each row); denom does not depend on the row. */
	int64_t ddx = 16 * dx;
	int64_t sq  = floor_div(ddx, denom);
	int64_t sr  = ddx - sq * denom;

	e->quot      = (int32_t)q;
	e->rem       = (int32_t)r;
	e->step_quot = (int32_t)sq;
	e->step_rem  = (int32_t)sr;
	e->denom     = (int32_t)denom;
	edge_dda_band_step(e);
}

/*
 * dy < 0 edge: `quot` is px_min, the smallest px with px_c >= K/dy (tie
 * included -- is_top_left() is always true for a dy < 0 edge, see its own
 * comment). Smallest-px-with-px->=-real-bound is a ceiling division;
 * rather than a separate ceil_div() this bakes the standard
 * ceil(a/b) == floor((a+b-1)/b) identity straight into the numerator
 * (denom - 1 added once, here, at setup) so the SAME floor_div()-based
 * quotient/remainder stepping as the upper case works unchanged.
 */
static void edge_dda_init_lower(edge_dda_t *e, tr_sv_t a, tr_sv_t b, int64_t dy, int32_t pcy0)
{
	int64_t dx    = (int64_t)(b.x - a.x);
	int64_t K     = dx * (int64_t)(pcy0 - a.y) + dy * (int64_t)a.x;
	int64_t denom = -16 * dy;                 /* > 0, since dy < 0 */
	int64_t numer = (8 * dy - K) + denom - 1; /* == -(K - 8*dy) + (denom - 1) */
	int64_t q     = floor_div(numer, denom);
	int64_t r     = numer - q * denom;
	/* K increases by dx*16 per row -> (8*dy - K) decreases by dx*16 per
	 * row -> numer's per-row delta is -dx*16 (the "+ denom - 1" term is
	 * row-independent). */
	int64_t ddx = -16 * dx;
	int64_t sq  = floor_div(ddx, denom);
	int64_t sr  = ddx - sq * denom;

	e->quot      = (int32_t)q;
	e->rem       = (int32_t)r;
	e->step_quot = (int32_t)sq;
	e->step_rem  = (int32_t)sr;
	e->denom     = (int32_t)denom;
	edge_dda_band_step(e);
}

/* Advance to the next row: rem and step_rem are each already in
 * [0, denom), so their sum is < 2*denom -- at most one correction is ever
 * needed, same invariant a textbook Bresenham line stepper relies on. */
static inline void edge_dda_step(edge_dda_t *e)
{
	e->quot += e->step_quot;
	e->rem += e->step_rem;
	if (e->rem >= e->denom) {
		e->rem -= e->denom;
		e->quot += 1;
	}
}

/* Advance n rows: whole bands by (q32, r32), the rest singly. */
static void edge_dda_skip(edge_dda_t *e, int32_t n)
{
	for (; n >= TR_BAND_H; n -= TR_BAND_H) {
		e->quot += e->q32;
		e->rem += e->r32;
		if (e->rem >= e->denom) {
			e->rem -= e->denom;
			e->quot += 1;
		}
	}
	for (; n > 0; n--) {
		edge_dda_step(e);
	}
}

const uint16_t *tr_r3d_tex[TR_TEX_MAX];
tr_tex_fog_t    tr_r3d_tex_fog[TR_TEX_MAX];
uint8_t         tr_r3d_fog_lut[TR_FOG_LUT_N];

#ifdef TR_RASTER_PROF
#define PROF_T0()            uint32_t prof_t0 = tr_prof_now()
#define PROF_ADD(k, px, cnt) prof_add((k), prof_t0, (px), (cnt))
static inline void prof_add(int k, uint32_t t0, uint32_t px, uint32_t cnt)
{
	tr_prof_t *p = &tr_prof_core()[k];

	p->cyc += tr_prof_now() - t0;
	p->px += px;
	p->n += cnt;
}
#else
#define PROF_T0()            ((void)0)
#define PROF_ADD(k, px, cnt) ((void)0)
#endif

/*
 * (a * b) >> sh for a < 2^63, b < 2^63, 32 <= sh < 64, via four 32x32->64
 * products (ARMv7/v8-A32 and M55 have no 64x64->128 multiply and GCC has no
 * __int128 there). Saturates at 2^62 when the true result does not fit --
 * only a sub-pixel sliver with a steep attribute gets there, and its pixels
 * (if any) are garbage either way. Per-triangle setup only.
 */
static uint64_t mul_shr(uint64_t a, uint64_t b, int sh)
{
	uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
	uint64_t lo = a0 * b0, m1 = a1 * b0, m2 = a0 * b1;
	uint64_t t  = (lo >> 32) + (m1 & 0xFFFFFFFFu) + (m2 & 0xFFFFFFFFu);
	uint64_t hi = a1 * b1 + (m1 >> 32) + (m2 >> 32) + (t >> 32);
	uint64_t rl = (lo & 0xFFFFFFFFu) | (t << 32);

	if ((hi >> (sh - 2)) != 0) {
		return (uint64_t)1 << 62;
	}
	return (hi << (64 - sh)) | (rl >> sh);
}

static int64_t smul_shr(int64_t a, uint64_t b, int sh)
{
	return a < 0 ? -(int64_t)mul_shr((uint64_t)-a, b, sh) : (int64_t)mul_shr((uint64_t)a, b, sh);
}

/*
 * floor(2^62 / a) for a > 0 -- tri_setup()'s per-triangle R, bit-exact with
 * the uint64 divide (a libgcc __aeabi_uldivmod call on the A32): a VFP double
 * estimate, then one correction step. For 2^10 <= a < 2^52, a is exact in a
 * double and the true quotient x = 2^62 / a < 2^52, so N = floor(x) and N + 1
 * are representable and the round-to-nearest divide gives N <= qd <= N + 1
 * (rounding is monotonic); q = trunc(qd) is N or N + 1, i.e. the remainder
 * r = 2^62 - q * a has -a <= r < a, and one "r < 0: q - 1" makes it exact
 * (review: 3.19M samples, never low). a < 2^10 (a sub-2-px^2 sliver) takes
 * the divide. Only a double-precision FPU uses
 * the estimate; elsewhere it is the divide. Silicon (a32/payload-isa,
 * 2026-09-23): 121 -> 84 cycles. tr_raster_selfcheck() compares the two.
 */
static uint64_t recip62(uint64_t a)
{
#if (defined(__ARM_FP) && (__ARM_FP & 8)) || defined(__x86_64__) || defined(__i386__) || \
    defined(__aarch64__)
	if (a >= 1024u && (a >> 52) == 0) {
		double   da = (double)(uint32_t)(a >> 32) * 4294967296.0 + (double)(uint32_t)a;
		double   qd = 4611686018427387904.0 / da; /* 2^62 */
		uint32_t hi = (uint32_t)(qd * (1.0 / 4294967296.0));
		uint64_t q  = (uint64_t)hi << 32 | (uint32_t)(qd - (double)hi * 4294967296.0);
		int64_t  r  = (int64_t)(((uint64_t)1 << 62) - q * a); /* -a <= r < a */

		return r < 0 ? q - 1u : q;
	}
#endif
	return ((uint64_t)1 << 62) / a;
}

typedef tr_plane_t plane_t;

/*
 * One attribute's screen-space plane (tr_plane_t), in uint32 MODULAR fixed
 * point: value(px, py) = C + gx*(px - ax) + gy*(py - ay), evaluated at
 * pixel centres. Every per-row/per-pixel step is a wrapping uint32 add (or
 * one uint32 multiply at span start), so the value at a pixel is the same
 * integer no matter which row the walk started from. Wrap-around is
 * harmless: a pixel inside the triangle has an in-range true value, and
 * modular arithmetic reproduces it exactly.
 *
 * a0..a2: the attribute at t's 3 vertices, in its fixed-point format
 * (|a| < 2^32). R = floor(2^62 / area2), computed once per triangle: the
 * ONLY division here. Gradient per pixel = num * 16 / area2; carried with
 * 8 extra fraction bits (hp) so C at the anchor is exact to ~2^-8 LSB per
 * pixel of anchor distance (<= 0.002 LSB-unit even from a guard-band
 * vertex), then rounded to the per-pixel uint32 step.
 */
static void plane_init(plane_t        *p,
                       int64_t         a0,
                       int64_t         a1,
                       int64_t         a2,
                       const tr_tri_t *t,
                       uint64_t        R,
                       int32_t         ax,
                       int32_t         ay)
{
	tr_sv_t  a = t->v[0], b = t->v[1], c = t->v[2];
	int64_t  d1 = a1 - a0, d2 = a2 - a0;
	int64_t  nx    = d1 * (int64_t)(c.y - a.y) - d2 * (int64_t)(b.y - a.y);
	int64_t  ny    = d2 * (int64_t)(b.x - a.x) - d1 * (int64_t)(c.x - a.x);
	int64_t  gx_hp = smul_shr(nx, R, 62 - 4 - 8);
	int64_t  gy_hp = smul_shr(ny, R, 62 - 4 - 8);
	uint64_t dx    = (uint64_t)(int64_t)(ax * SUB_ONE + SUB_HALF - a.x);
	uint64_t dy    = (uint64_t)(int64_t)(ay * SUB_ONE + SUB_HALF - a.y);
	/* wrapping uint64 products: bits 12..43 (all C needs) stay exact */
	uint64_t off = (uint64_t)gx_hp * dx + (uint64_t)gy_hp * dy + (1u << 11);

	p->row = (uint32_t)a0 + (uint32_t)(off >> 12);
	p->gx  = (uint32_t)(((uint64_t)gx_hp + 128u) >> 8);
	p->gy  = (uint32_t)(((uint64_t)gy_hp + 128u) >> 8);
}

/* Plane slots: w always; r,g,b for Gouraud, or U,V in the same slots for
 * textures (TR_TRI_TEX wins over GOURAUD, so a triangle never needs both):
 * a textured row advances 3 planes, a Gouraud row 4. */
enum { PL_W, PL_R, PL_G, PL_B, PL_N, PL_U = PL_R, PL_V = PL_G };

/*
 * Narrows rows [*y0, *y1) to the ones that can hold a pixel on screen: the y
 * extent of the triangle clipped to the strip 0 <= x <= TR_R3D_W (28.4) --
 * its vertices inside the strip plus its edges' crossings of the two strip
 * lines, rounded outward. A row outside that extent has no pixel centre in
 * [0, TR_R3D_W), so dropping it changes no pixel; it only stops the row loop
 * stepping a wall cap or a ground column that is off the side of the screen
 * (measured on the golden scene: 43 % of all triangle-rows belonged to
 * triangles wholly off a side, 54 % drew nothing). False: no row left.
 * Setup only (one int64 divide per crossing).
 */
static bool strip_rows(const tr_sv_t v[3], int32_t *y0, int32_t *y1)
{
	const int32_t xr = TR_R3D_W * SUB_ONE;
	int64_t       lo = INT64_MAX, hi = INT64_MIN;

	for (int i = 0; i < 3; i++) {
		tr_sv_t a = v[i], b = v[i == 2 ? 0 : i + 1];

		if (a.x >= 0 && a.x <= xr) {
			lo = a.y < lo ? a.y : lo;
			hi = a.y > hi ? a.y : hi;
		}
		for (int32_t X = 0; X <= xr; X += xr) {
			if ((a.x < X && b.x > X) || (a.x > X && b.x < X)) {
				int64_t dx  = (int64_t)b.x - a.x;
				int64_t num = (int64_t)(X - a.x) * (int64_t)(b.y - a.y);
				int64_t y;

				if (dx < 0) {
					dx = -dx, num = -num;
				}
				y  = a.y + floor_div(num, dx); /* crossing is in [y, y + 1) */
				lo = y < lo ? y : lo;
				hi = y + 1 > hi ? y + 1 : hi;
			}
		}
	}
	if (lo > hi) {
		return false; /* wholly left or right of the screen */
	}

	int32_t r0 = (int32_t)(lo >> TR_R3D_SUB);                 /* <= first row with centre >= lo */
	int32_t r1 = (int32_t)((hi + SUB_ONE - 1) >> TR_R3D_SUB); /* > last row with centre <= hi */

	*y0 = r0 > *y0 ? r0 : *y0;
	*y1 = r1 < *y1 ? r1 : *y1;
	return *y0 < *y1;
}

/*
 * The whole per-triangle setup (all its divides): exact rows [y0, y1), the
 * non-horizontal edges' DDAs at row y0, and -- with `planes` -- the
 * attribute planes anchored at (ax, y0). Returns false when nothing is
 * drawn (backface/degenerate, or no row on screen).
 *
 * A horizontal edge (dy == 0) passes or fails whole rows (its edge value
 * does not depend on x), so it folds into the row range instead of a
 * per-row gate: the top edge (runs +x, top-left INCLUSIVE) admits rows with
 * pcy >= a.y, the bottom edge (runs -x, exclusive) rows with pcy < a.y;
 * both boundaries are row ceil((a.y - 8) / 16) == (a.y + 7) >> 4.
 */
static bool tri_setup(tr_tri_setup_t *s, const tr_tri_t *t, bool planes)
{
	tr_sv_t a = t->v[0], b = t->v[1], c = t->v[2];
	int64_t area2 = edge(a, b, c);

	s->y0 = s->y1 = 0;
	if (area2 <= 0) {
		return false; /* backface or degenerate */
	}

	/* Y bounding box, clamped to the panel (28.4 -> px is an arithmetic
	 * shift, which floors correctly for negative coordinates). */
	int32_t miny = a.y, maxy = a.y, minx = a.x, maxx = a.x;

	miny = b.y < miny ? b.y : miny;
	maxy = b.y > maxy ? b.y : maxy;
	miny = c.y < miny ? c.y : miny;
	maxy = c.y > maxy ? c.y : maxy;
	minx = b.x < minx ? b.x : minx;
	minx = c.x < minx ? c.x : minx;
	maxx = b.x > maxx ? b.x : maxx;
	maxx = c.x > maxx ? c.x : maxx;

	/* No pixel centre (16 px + 8 on each axis) inside the closed bounding
	 * box, or no centre column on screen: no pixel can be inside (a pixel
	 * is drawn only if its centre is in the triangle or on an edge), so
	 * nothing to set up. Thin slivers, which the row range alone would still
	 * bin (~100 a frame drew nothing, algorithm study #10). */
	int32_t cx0 = (minx - SUB_HALF + SUB_ONE - 1) >> TR_R3D_SUB; /* first centre >= minx */
	int32_t cx1 = (maxx - SUB_HALF) >> TR_R3D_SUB;               /* last centre <= maxx */
	int32_t cy0 = (miny - SUB_HALF + SUB_ONE - 1) >> TR_R3D_SUB;
	int32_t cy1 = (maxy - SUB_HALF) >> TR_R3D_SUB;

	if (cx0 > cx1 || cy0 > cy1 || cx1 < 0 || cx0 >= TR_R3D_W) {
		return false;
	}

	/* fix round 8: TR_VIEW_H, not TR_R3D_H, for the BINNED path -- REQUIRED,
	 * not just an optimisation: tr_bin_build()'s bins[TR_BANDS][...] array
	 * is sized to the shrunk TR_BANDS (27, r3d.h), so a span reaching into
	 * "phantom" bands 27..39 (which do not exist in that array any more)
	 * corrupts memory past it. Reverting this to TR_R3D_H unconditionally
	 * segfaulted test_r3d_scene.c/test_r3d_zones.c immediately (confirmed)
	 * -- this clip is load-bearing for the new TR_BANDS, not optional.
	 *
	 * fix round 12 (review, minor): that TR_VIEW_H clip does NOT apply to
	 * tr_raster_tri()'s own call (tri_setup(..., false), this function's
	 * `planes` param is exactly the binned-vs-not distinction: tr_tri_
	 * setup_range() -- the BINNED path -- passes true; tr_raster_tri()
	 * passes false). tr_raster_tri()'s own documented contract is the
	 * FULL TR_R3D_H screen (test_r3d_raster.c's header comment) and it
	 * never touches tr_bin_build/BINS at all (its raster_setup call
	 * passes bins=NULL), so clipping it to TR_VIEW_H too was simply
	 * wrong for that caller -- it silently could not draw anything in
	 * rows [TR_VIEW_H, TR_R3D_H), contradicting its own contract. An
	 * earlier round found this via test_r3d_raster.c's own bottom-edge
	 * case (corner_y[4]) failing near TR_R3D_H and moved the test point
	 * away from the boundary instead of fixing the clip -- fixed properly
	 * now: `planes` selects the right window per caller, and the test
	 * point moves back next to test_r3d_raster.c's own comment. */
	int32_t y_max = planes ? TR_VIEW_H : TR_R3D_H;
	int32_t y0    = clampi(miny >> TR_R3D_SUB, 0, y_max);
	int32_t y1    = clampi((maxy + SUB_ONE - 1) >> TR_R3D_SUB, 0, y_max);
	tr_sv_t ea[3] = { a, b, c }, eb[3] = { b, c, a };

	for (int i = 0; i < 3; i++) {
		if (eb[i].y == ea[i].y) {
			int32_t row = (ea[i].y + 7) >> TR_R3D_SUB;

			if (is_top_left(ea[i], eb[i])) {
				y0 = row > y0 ? row : y0;
			} else {
				y1 = row < y1 ? row : y1;
			}
		}
	}
	if (y0 >= y1) {
		return false; /* nothing on screen for this triangle */
	}

	/* Planes stay anchored at the unclipped first row yp (so every pixel's
	 * attribute is the same integer whatever the strip clip does); the edge
	 * DDAs start at the clipped y0 -- their init is an exact floor at any
	 * row, bit-identical to stepping there. */
	int32_t yp = y0;

	if ((minx < 0 || maxx > TR_R3D_W * SUB_ONE) && !strip_rows(t->v, &y0, &y1)) {
		return false;
	}

	int32_t pcy0 = (y0 << TR_R3D_SUB) + SUB_HALF;

	/* Right bounds first, then left: the row loop takes a min over
	 * e[0..nr) and a max over e[nr..ne) without a per-edge branch. */
	s->ne = 0;
	for (int i = 0; i < 3; i++) {
		int64_t dy = (int64_t)(eb[i].y - ea[i].y);

		if (dy > 0) {
			edge_dda_init_upper(&s->e[s->ne++], ea[i], eb[i], dy, pcy0);
		}
	}
	s->nr = s->ne;
	for (int i = 0; i < 3; i++) {
		int64_t dy = (int64_t)(eb[i].y - ea[i].y);

		if (dy < 0) {
			edge_dda_init_lower(&s->e[s->ne++], ea[i], eb[i], dy, pcy0);
		}
	}
	s->y0 = (int16_t)y0;
	s->y1 = (int16_t)y1;
	s->ax = (int16_t)clampi(minx >> TR_R3D_SUB, 0, TR_R3D_W);
	if (!planes) {
		return true;
	}

	/* One int64 division per triangle for R, then multiplies. */
	uint64_t R  = recip62((uint64_t)area2);
	int32_t  ax = s->ax;

	/* w in 16.16: half-LSB biased, or TR_TRI_UVX8's exact fraction bits */
	int64_t Wv[3];

	for (int k = 0; k < 3; k++) {
		Wv[k] = ((int64_t)t->a[k].w << 16) + ((t->flags & TR_TRI_UVX8) ? t->a[k].rgb : 0x8000);
	}
	plane_init(&s->pl[PL_W], Wv[0], Wv[1], Wv[2], t, R, ax, yp);
	if (t->flags & TR_TRI_TEX) {
		int64_t U[3], V[3], m = 0;
		int     us = (t->flags & TR_TRI_UVX8) ? 3 : 0;

		/* U = (u + 1/2 LSB) * w / 4: < 2^30, and 4U/w floors to exactly
		 * u at a vertex (the half-LSB bias absorbs the sub-LSB plane
		 * error). TR_TRI_UVX8: u is the stored value * 8, half-LSB biased
		 * at the real 8.8 scale, times the 16.16 w. U < 2^30 is then the
		 * flag's precondition (r3d.h: u * w stays small, as it does on a
		 * ground quad, whose u/v are large only where w is small); a
		 * triangle that breaks it is not drawn rather than drawn wrong. */
		for (int k = 0; k < 3; k++) {
			int64_t wk = (t->flags & TR_TRI_UVX8) ? Wv[k] : (int64_t)t->a[k].w << 16;

			U[k] = (2 * ((int64_t)t->a[k].u << us) + 1) * wk;
			V[k] = (2 * ((int64_t)t->a[k].v << us) + 1) * wk;
			m    = U[k] > m ? U[k] : m;
			m    = V[k] > m ? V[k] : m;
		}
		if ((m >> 19) >= ((int64_t)1 << 30)) {
			s->y0 = s->y1 = 0; /* only reachable with TR_TRI_UVX8 */
			return false;
		}
		for (int k = 0; k < 3; k++) {
			U[k] >>= 19;
			V[k] >>= 19;
		}
		plane_init(&s->pl[PL_U], U[0], U[1], U[2], t, R, ax, yp);
		plane_init(&s->pl[PL_V], V[0], V[1], V[2], t, R, ax, yp);
	} else if (t->flags & TR_TRI_GOURAUD) {
		static const uint8_t sh[3] = { 11, 5, 0 }, mk[3] = { 0x1F, 0x3F, 0x1F };

		for (int ch = 0; ch < 3; ch++) {
			int64_t v[3];

			for (int k = 0; k < 3; k++) {
				v[k] = ((int64_t)((t->a[k].rgb >> sh[ch]) & mk[ch]) << 16) + 0x8000;
			}
			plane_init(&s->pl[PL_R + ch], v[0], v[1], v[2], t, R, ax, yp);
		}
	}
	for (int k  = 0,
	         np = (t->flags & TR_TRI_TEX)       ? PL_V + 1
	              : (t->flags & TR_TRI_GOURAUD) ? PL_B + 1
	                                            : 1;
	     k < np;
	     k++) {
		s->pl[k].row += s->pl[k].gy * (uint32_t)(y0 - yp);
	}
	return true;
}

/* Perspective divide for one textured sub-span end: u = 4U / w, returned
 * in 8.11 texels (3 bits finer than the 8.8 vertex coords, so the floor to
 * a texel happens once, at the pixel). W (w in 16.16) is normalised by CLZ
 * to a 16-bit mantissa, so 1/w keeps ~15 significant bits at every depth
 * -- truncating w to an integer (the first version) lost up to 0.2% at
 * w ~ 227 (far): texel shimmer. One 32-bit divide (UDIV on A32 and M55) +
 * two 32x32->64 SMULLs; U < 2^30, rw <= 2^16 + 1. persp_w() then persp_uv(). */
typedef struct {
	int32_t rw; /* rounded 2^31 / (W normalised to 16 bits) */
	int     rs; /* the matching right shift */
} persp_t;

/* The W half of the perspective divide: all of its divide. A span whose w does not change
 * along x (pl[PL_W].gx == 0: the ground, ~99.9 % of its px) needs it once. */
static inline persp_t persp_w(uint32_t W)
{
	uint32_t wn = W | 1u;
	int      sh = __builtin_clz(wn); /* <= 15 for w >= 1 */
	uint32_t d  = (wn << sh) >> 16;  /* [2^15, 2^16) */
	persp_t  p;

	p.rw = (int32_t)((0x7FFFFFFFu + (d >> 1)) / d); /* rounded */
	p.rs = 26 - (sh > 25 ? 25 : sh);
	return p;
}

static inline void persp_uv(uint32_t U, uint32_t V, persp_t p, int32_t *u, int32_t *v)
{
	*u = (int32_t)(((int64_t)(int32_t)U * p.rw) >> p.rs);
	*v = (int32_t)(((int64_t)(int32_t)V * p.rw) >> p.rs);
}

/*
 * Z-tested span loops. The scalar forms are the reference (and the host /
 * M55 path); the NEON forms do 8 px per iteration with the same modular
 * arithmetic -- lane k of the w/r/g/b vectors is base + k*step, exactly the
 * scalar sequence -- and are checked against the scalar ones by
 * tr_raster_selfcheck(). dst/z never alias each other or the texture.
 */
static void span_flat_z_scalar(uint16_t *restrict dst,
                               uint16_t *restrict z,
                               int32_t  n,
                               uint32_t W,
                               uint32_t gw,
                               uint16_t c)
{
	for (int32_t i = 0; i < n; i++) {
		uint16_t w = (uint16_t)(W >> 16);

		if (w > z[i]) {
			z[i]   = w;
			dst[i] = c;
		}
		W += gw;
	}
}

static void span_gouraud_z_scalar(uint16_t *restrict dst,
                                  uint16_t *restrict z,
                                  int32_t        n,
                                  const uint32_t s[PL_N],
                                  const plane_t  pl[PL_N])
{
	uint32_t W = s[PL_W], R = s[PL_R], G = s[PL_G], B = s[PL_B];
	uint32_t gw = pl[PL_W].gx, gr = pl[PL_R].gx, gg = pl[PL_G].gx, gb = pl[PL_B].gx;

	for (int32_t i = 0; i < n; i++) {
		uint16_t w = (uint16_t)(W >> 16);

		if (w > z[i]) {
			z[i]   = w;
			dst[i] = (uint16_t)(((R >> 16) << 11) | ((G >> 16) << 5) | (B >> 16));
		}
		W += gw;
		R += gr;
		G += gg;
		B += gb;
	}
}

/* TR_TRI_NOZ Gouraud: the same colours, no z read or write. */
static void span_gouraud_scalar(uint16_t *restrict dst,
                                int32_t        n,
                                const uint32_t s[PL_N],
                                const plane_t  pl[PL_N])
{
	uint32_t R = s[PL_R], G = s[PL_G], B = s[PL_B];

	for (int32_t i = 0; i < n; i++) {
		dst[i] = (uint16_t)(((R >> 16) << 11) | ((G >> 16) << 5) | (B >> 16));
		R += pl[PL_R].gx;
		G += pl[PL_G].gx;
		B += pl[PL_B].gx;
	}
}

#if defined(__ARM_NEON)

#include <arm_neon.h>

/* {base, base+g, base+2g, base+3g} and the next 4 lanes. */
static inline void lanes(uint32_t base, uint32_t g, uint32x4_t *lo, uint32x4_t *hi)
{
	static const uint32_t k[4] = { 0, 1, 2, 3 };

	*lo = vmlaq_n_u32(vdupq_n_u32(base), vld1q_u32(k), g);
	*hi = vaddq_u32(*lo, vdupq_n_u32(g * 4u));
}

static inline uint16x8_t hi16(uint32x4_t lo, uint32x4_t hi)
{
	return vcombine_u16(vshrn_n_u32(lo, 16), vshrn_n_u32(hi, 16));
}

/*
 * n >= 8: 8-px NEON chunks. A ragged end is the next chunk with its lanes
 * >= n masked off (z and colour of those lanes stored back as loaded) when
 * that chunk still lies inside the row -- `room` px from dst/z to the end of
 * the band row, so it never leaves its own buffer or reaches another core's
 * -- and otherwise one more chunk ENDING at pixel n - 1, overlapping the
 * previous one. Overlapped pixels are exactly the ones the previous chunk
 * just z-tested: a written one now has z == w, an unwritten one z >= w, so
 * `w > z` fails for both and nothing changes. Either way the result is the
 * scalar loop's, and nothing outside [0, n) changes. Silicon (a32/payload-isa,
 * 2026-09-23): the masked end took a 9-12 px Gouraud span from ~255 to ~193
 * cycles. n < 8: the scalar loop (no vector setup to repay; masking a single
 * chunk measured slower there).
 */
static const uint16_t lane_idx[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

/* Mask m to its first `left` lanes (left < 8). */
static inline uint16x8_t lanes_below(uint16x8_t m, int32_t left)
{
	return vandq_u16(m, vcltq_u16(vld1q_u16(lane_idx), vdupq_n_u16((uint16_t)left)));
}

static inline __attribute__((always_inline)) void span_flat_z(uint16_t *restrict dst,
                                                              uint16_t *restrict z,
                                                              int32_t  n,
                                                              uint32_t W,
                                                              uint32_t gw,
                                                              uint16_t c,
                                                              int32_t  room)
{
	if (n < 8) {
		span_flat_z_scalar(dst, z, n, W, gw, c);
		return;
	}

	uint32x4_t w0, w1, st = vdupq_n_u32(gw * 8u);
	uint16x8_t cv = vdupq_n_u16(c);
	int32_t    i  = 0;

	lanes(W, gw, &w0, &w1);
	for (;;) {
		uint16x8_t w  = hi16(w0, w1);
		uint16x8_t zo = vld1q_u16(&z[i]);
		uint16x8_t m  = vcgtq_u16(w, zo);

		RASTER_CHECK(i + 8 <= room); /* this chunk stays inside the row */
		if (n - i < 8) {
			m = lanes_below(m, n - i);
		}
		vst1q_u16(&z[i], vbslq_u16(m, w, zo));
		vst1q_u16(&dst[i], vbslq_u16(m, cv, vld1q_u16(&dst[i])));
		i += 8;
		if (i >= n) {
			return;
		}
		if (i + 8 > n && i + 8 > room) {
			i = n - 8;
			lanes(W + gw * (uint32_t)i, gw, &w0, &w1);
		} else {
			w0 = vaddq_u32(w0, st), w1 = vaddq_u32(w1, st);
		}
	}
}

static inline __attribute__((always_inline)) void span_gouraud_z(uint16_t *restrict dst,
                                                                 uint16_t *restrict z,
                                                                 int32_t        n,
                                                                 const uint32_t s[PL_N],
                                                                 const plane_t  pl[PL_N],
                                                                 int32_t        room)
{
	if (n < 8) {
		span_gouraud_z_scalar(dst, z, n, s, pl);
		return;
	}

	const uint32_t gw = pl[PL_W].gx, gr = pl[PL_R].gx, gg = pl[PL_G].gx, gb = pl[PL_B].gx;
	uint32x4_t     w0, w1, r0, r1, g0, g1, b0, b1;
	uint32x4_t     sw = vdupq_n_u32(gw * 8u), sr = vdupq_n_u32(gr * 8u);
	uint32x4_t     sg = vdupq_n_u32(gg * 8u), sb = vdupq_n_u32(gb * 8u);
	int32_t        i = 0;

	lanes(s[PL_W], gw, &w0, &w1);
	lanes(s[PL_R], gr, &r0, &r1);
	lanes(s[PL_G], gg, &g0, &g1);
	lanes(s[PL_B], gb, &b0, &b1);
	for (;;) {
		uint16x8_t w   = hi16(w0, w1);
		uint16x8_t zo  = vld1q_u16(&z[i]);
		uint16x8_t m   = vcgtq_u16(w, zo);
		uint16x8_t col = vorrq_u16(
		    vorrq_u16(vshlq_n_u16(hi16(r0, r1), 11), vshlq_n_u16(hi16(g0, g1), 5)), hi16(b0, b1));

		RASTER_CHECK(i + 8 <= room); /* this chunk stays inside the row */
		if (n - i < 8) {
			m = lanes_below(m, n - i);
		}
		vst1q_u16(&z[i], vbslq_u16(m, w, zo));
		vst1q_u16(&dst[i], vbslq_u16(m, col, vld1q_u16(&dst[i])));
		i += 8;
		if (i >= n) {
			return;
		}
		if (i + 8 > n && i + 8 > room) {
			uint32_t k = (uint32_t)(i = n - 8);

			lanes(s[PL_W] + gw * k, gw, &w0, &w1);
			lanes(s[PL_R] + gr * k, gr, &r0, &r1);
			lanes(s[PL_G] + gg * k, gg, &g0, &g1);
			lanes(s[PL_B] + gb * k, gb, &b0, &b1);
		} else {
			w0 = vaddq_u32(w0, sw), w1 = vaddq_u32(w1, sw);
			r0 = vaddq_u32(r0, sr), r1 = vaddq_u32(r1, sr);
			g0 = vaddq_u32(g0, sg), g1 = vaddq_u32(g1, sg);
			b0 = vaddq_u32(b0, sb), b1 = vaddq_u32(b1, sb);
		}
	}
}

/* NOZ Gouraud, 8 px a chunk; the ragged end as span_flat_z() (the
 * overlapped form rewrites the overlapped pixels with the values they hold). */
static inline __attribute__((always_inline)) void span_gouraud(uint16_t *restrict dst,
                                                               int32_t        n,
                                                               const uint32_t s[PL_N],
                                                               const plane_t  pl[PL_N],
                                                               int32_t        room)
{
	if (n < 8) {
		span_gouraud_scalar(dst, n, s, pl);
		return;
	}

	const uint32_t gr = pl[PL_R].gx, gg = pl[PL_G].gx, gb = pl[PL_B].gx;
	uint32x4_t     r0, r1, g0, g1, b0, b1;
	uint32x4_t     sr = vdupq_n_u32(gr * 8u), sg = vdupq_n_u32(gg * 8u), sb = vdupq_n_u32(gb * 8u);
	int32_t        i = 0;

	lanes(s[PL_R], gr, &r0, &r1);
	lanes(s[PL_G], gg, &g0, &g1);
	lanes(s[PL_B], gb, &b0, &b1);
	for (;;) {
		uint16x8_t col = vorrq_u16(
		    vorrq_u16(vshlq_n_u16(hi16(r0, r1), 11), vshlq_n_u16(hi16(g0, g1), 5)), hi16(b0, b1));

		RASTER_CHECK(i + 8 <= room); /* this chunk stays inside the row */
		if (n - i < 8) {
			uint16x8_t m = lanes_below(vdupq_n_u16(0xFFFFu), n - i);

			col = vbslq_u16(m, col, vld1q_u16(&dst[i]));
		}
		vst1q_u16(&dst[i], col);
		i += 8;
		if (i >= n) {
			return;
		}
		if (i + 8 > n && i + 8 > room) {
			uint32_t k = (uint32_t)(i = n - 8);

			lanes(s[PL_R] + gr * k, gr, &r0, &r1);
			lanes(s[PL_G] + gg * k, gg, &g0, &g1);
			lanes(s[PL_B] + gb * k, gb, &b0, &b1);
		} else {
			r0 = vaddq_u32(r0, sr), r1 = vaddq_u32(r1, sr);
			g0 = vaddq_u32(g0, sg), g1 = vaddq_u32(g1, sg);
			b0 = vaddq_u32(b0, sb), b1 = vaddq_u32(b1, sb);
		}
	}
}

#else

/* No NEON: `room` (the masked-end bound) is unused. */
#define span_flat_z(d, z, n, W, gw, c, room) span_flat_z_scalar(d, z, n, W, gw, c)
#define span_gouraud_z(d, z, n, s, pl, room) span_gouraud_z_scalar(d, z, n, s, pl)
#define span_gouraud(d, n, s, pl, room)      span_gouraud_scalar(d, n, s, pl)

#endif

/* Textured, z-tested span: perspective-correct at sub-span ends (one
 * divide each), affine (8.14 fixed) inside. A sub-span's end sample is the
 * next sub-span's first pixel, or -- for the last one -- the span's own
 * last pixel: never a pixel past the span, where the w plane can run
 * through 0 off the triangle. The affine step for a k < 8 px end costs one
 * extra 32-bit divide per span.
 *
 * The pixels of one sub-span: scalar reference, and (NEON) 8 at a time --
 * lane k is the scalar sequence's k-th value (wrapping uint32 adds), the 8
 * texel indices are computed in vectors and gathered with scalar loads
 * (every index is masked into the texture, so all 8 loads are in bounds
 * even where the z test fails). tr_raster_selfcheck() compares the two. */
static inline void tex_run_scalar(uint16_t *restrict dst,
                                  uint16_t *restrict z,
                                  int32_t  m,
                                  uint32_t W,
                                  uint32_t gw,
                                  uint32_t ua,
                                  uint32_t du,
                                  uint32_t va,
                                  uint32_t dv,
                                  const uint16_t *restrict tex)
{
	for (int32_t i = 0; i < m; i++) {
		uint16_t w = (uint16_t)(W >> 16);

		if (w > z[i]) {
			z[i]   = w;
			dst[i] = tex[(((va >> 14) & (TR_TEX_DIM - 1)) << 7) | ((ua >> 14) & (TR_TEX_DIM - 1))];
		}
		W += gw;
		ua += du;
		va += dv;
	}
}

/* TR_TRI_NOZ texels: no z, so no vector compare/select to feed -- a plain
 * core-register loop beats tex_run8(), whose cost is moving the 8 lane
 * offsets out of NEON and the texels back in. The texel's BYTE offset is
 * formed directly (tex_run8()'s trick: ((v >> 14) & 127) << 8 | ((u >> 14) &
 * 127) << 1), so the A32 needs LSR, AND, LSR, AND, ORR, LDRH, STRH and the two
 * ADDs a pixel (70 instructions per 8-px sub-span, fully unrolled, no
 * spills). Same texel sequence as tex_run_scalar(). */
static inline __attribute__((always_inline)) void tex_run_noz(uint16_t *restrict dst,
                                                              int32_t  m,
                                                              uint32_t ua,
                                                              uint32_t du,
                                                              uint32_t va,
                                                              uint32_t dv,
                                                              const uint16_t *restrict tex)
{
	const uint8_t *tb = (const uint8_t *)tex;

	_Static_assert(TR_TEX_DIM == 128, "the >> 6 / >> 13 below place a 128-texel row at 256 B");
#pragma GCC unroll 8
	for (int32_t i = 0; i < m; i++) {
		uint32_t o = ((va >> 6) & ((TR_TEX_DIM - 1) << 8)) | ((ua >> 13) & ((TR_TEX_DIM - 1) << 1));

		dst[i] = *(const uint16_t *)(const void *)(tb + o);
		ua += du;
		va += dv;
	}
}

/* tex_run_noz() through a fogged palette row: the texel is a byte, idx[] a
 * byte offset into `pal` (entry * 2), so the A32 adds just one LDRB. */
static inline __attribute__((always_inline)) void tex_run_noz_fog(uint16_t *restrict dst,
                                                                  int32_t  m,
                                                                  uint32_t ua,
                                                                  uint32_t du,
                                                                  uint32_t va,
                                                                  uint32_t dv,
                                                                  const uint8_t *restrict idx,
                                                                  const uint8_t *restrict pal)
{
#pragma GCC unroll 8
	for (int32_t i = 0; i < m; i++) {
		uint32_t o = ((va >> 7) & ((TR_TEX_DIM - 1) << 7)) | ((ua >> 14) & (TR_TEX_DIM - 1));

		dst[i] = *(const uint16_t *)(const void *)(pal + idx[o]);
		ua += du;
		va += dv;
	}
}

/* Fog level of a NOZ textured sub-span starting at w (16.16). */
static inline uint32_t fog_level(uint32_t W)
{
	uint32_t i = W >> (16 + TR_FOG_W_SHIFT);

	return i < TR_FOG_LUT_N ? tr_r3d_fog_lut[i] : 0u;
}

#if defined(__ARM_NEON)
static inline __attribute__((always_inline)) void tex_run8(uint16_t *restrict dst,
                                                           uint16_t *restrict z,
                                                           uint32_t W,
                                                           uint32_t gw,
                                                           uint32_t ua,
                                                           uint32_t du,
                                                           uint32_t va,
                                                           uint32_t dv,
                                                           const uint16_t *restrict tex)
{
	uint32x4_t w0, w1, u0, u1, v0, v1;
	lanes(W, gw, &w0, &w1);
	lanes(ua, du, &u0, &u1);
	lanes(va, dv, &v0, &v1);
	/* Texel BYTE offset ((v >> 14) & 127) << 8 | ((u >> 14) & 127) << 1
	 * == ((v >> 14) & 127) << 8 | ((u >> 13) & 0xFE), in 16-bit lanes (the
	 * narrowing drops only bits the masks drop anyway); moved to core
	 * registers two lanes per word (vmov r, r, d), gathered, and packed
	 * back two texels per word. */
	uint16x8_t uh = vcombine_u16(vshrn_n_u32(u0, 13), vshrn_n_u32(u1, 13));
	uint16x8_t vh = vcombine_u16(vshrn_n_u32(v0, 14), vshrn_n_u32(v1, 14));
	uint16x8_t of = vorrq_u16(vshlq_n_u16(vandq_u16(vh, vdupq_n_u16(TR_TEX_DIM - 1)), 8),
	                          vandq_u16(uh, vdupq_n_u16((TR_TEX_DIM - 1) << 1)));
	uint64x2_t ix = vreinterpretq_u64_u16(of);
	uint64_t   a = vgetq_lane_u64(ix, 0), b = vgetq_lane_u64(ix, 1);
	uint32_t q0 = (uint32_t)a, q1 = (uint32_t)(a >> 32), q2 = (uint32_t)b, q3 = (uint32_t)(b >> 32);
	const uint8_t *tb = (const uint8_t *)tex;
#define TEXEL(o) ((uint32_t)*(const uint16_t *)(const void *)(tb + (o)))
	uint64_t t0 = (uint64_t)(TEXEL(q0 & 0xFFFFu) | TEXEL(q0 >> 16) << 16) |
	              (uint64_t)(TEXEL(q1 & 0xFFFFu) | TEXEL(q1 >> 16) << 16) << 32;
	uint64_t t1 = (uint64_t)(TEXEL(q2 & 0xFFFFu) | TEXEL(q2 >> 16) << 16) |
	              (uint64_t)(TEXEL(q3 & 0xFFFFu) | TEXEL(q3 >> 16) << 16) << 32;
#undef TEXEL
	uint16x8_t tv = vcombine_u16(vcreate_u16(t0), vcreate_u16(t1));

	uint16x8_t w  = hi16(w0, w1);
	uint16x8_t zo = vld1q_u16(z);
	uint16x8_t m  = vcgtq_u16(w, zo);

	vst1q_u16(z, vmaxq_u16(w, zo));
	vst1q_u16(dst, vbslq_u16(m, tv, vld1q_u16(dst)));
}
#endif

static inline __attribute__((always_inline)) void span_tex_z_impl(uint16_t *restrict dst,
                                                                  uint16_t *restrict z,
                                                                  int32_t        n,
                                                                  const uint32_t s[PL_N],
                                                                  const plane_t  pl[PL_N],
                                                                  const uint16_t *restrict tex,
                                                                  int                 vec,
                                                                  const int           noz,
                                                                  const tr_tex_fog_t *fog)
{
	uint32_t W = s[PL_W], U = s[PL_U], V = s[PL_V], Wd = W;
	uint32_t gw = pl[PL_W].gx, gu = pl[PL_U].gx, gv = pl[PL_V].gx;
	int32_t  us, vs, ue, ve;
	persp_t  pw = persp_w(Wd);

	(void)vec;
	persp_uv(U, V, pw, &us, &vs);
	for (int32_t x = 0; x < n; x += TR_TEX_SUB) {
		int32_t m = (n - x < TR_TEX_SUB) ? n - x : TR_TEX_SUB;
		int32_t k = (x + m < n) ? m : m - 1; /* px from x to the end sample */

		U += gu * (uint32_t)k;
		V += gv * (uint32_t)k;
		Wd += gw * (uint32_t)k;
		pw = persp_w(Wd);
		persp_uv(U, V, pw, &ue, &ve);

		/* 8.14 per pixel == (end - start) in 8.11 * 8 / k; k ==
		 * TR_TEX_SUB (the common case) divides by a constant. */
		uint32_t ua = (uint32_t)us << 3, va = (uint32_t)vs << 3;
		uint32_t du, dv;

		if (k == TR_TEX_SUB) {
			du = (uint32_t)((ue - us) * 8 / TR_TEX_SUB);
			dv = (uint32_t)((ve - vs) * 8 / TR_TEX_SUB);
		} else {
			du = k ? (uint32_t)((ue - us) * 8 / k) : 0;
			dv = k ? (uint32_t)((ve - vs) * 8 / k) : 0;
		}
		if (noz) {
			uint32_t lv = fog ? fog_level(W) : 0u;

			if (lv != 0 ||
			    tex == NULL) { /* NULL: an index-only texture, level 0 through pal too (r3d.h) */
				const uint8_t *pal = (const uint8_t *)(fog->pal + lv * fog->npal);

				if (m == TR_TEX_SUB) {
					tex_run_noz_fog(&dst[x], TR_TEX_SUB, ua, du, va, dv, fog->idx, pal);
				} else {
					tex_run_noz_fog(&dst[x], m, ua, du, va, dv, fog->idx, pal);
				}
			} else if (m == TR_TEX_SUB) { /* constant trip count: unrolled */
				tex_run_noz(&dst[x], TR_TEX_SUB, ua, du, va, dv, tex);
			} else {
				tex_run_noz(&dst[x], m, ua, du, va, dv, tex);
			}
		} else {
#if TR_TEX_SUB == 8
			/* One 8-px chunk per sub-span. */
#if defined(__ARM_NEON)
			if (vec && m == 8) {
				tex_run8(&dst[x], &z[x], W, gw, ua, du, va, dv, tex);
			} else if (vec && n >= 8) {
				/* Ragged end: one chunk ending at pixel n - 1, lanes backed
			 * up by 8 - m with this sub-span's own steps. The overlapped
			 * pixels were z-tested already, so they cannot change (see
			 * span_flat_z()). */
				uint32_t bk = 8u - (uint32_t)m;

				tex_run8(&dst[n - 8],
				         &z[n - 8],
				         W - gw * bk,
				         gw,
				         ua - du * bk,
				         du,
				         va - dv * bk,
				         dv,
				         tex);
			} else
#endif
			{
				tex_run_scalar(&dst[x], &z[x], m, W, gw, ua, du, va, dv, tex);
			}
#else
			for (int32_t j = 0; j < m; j += 8) {
				int32_t  c  = m - j < 8 ? m - j : 8;
				uint32_t Wj = W + gw * (uint32_t)j, uj = ua + du * (uint32_t)j,
				         vj = va + dv * (uint32_t)j;

#if defined(__ARM_NEON)
				if (vec && c == 8) {
					tex_run8(&dst[x + j], &z[x + j], Wj, gw, uj, du, vj, dv, tex);
					continue;
				}
				if (vec && x + j + c >= 8) { /* ragged end, as above */
					uint32_t bk = 8u - (uint32_t)c;

					tex_run8(&dst[x + j + c - 8],
					         &z[x + j + c - 8],
					         Wj - gw * bk,
					         gw,
					         uj - du * bk,
					         du,
					         vj - dv * bk,
					         dv,
					         tex);
					continue;
				}
#endif
				tex_run_scalar(&dst[x + j], &z[x + j], c, Wj, gw, uj, du, vj, dv, tex);
			}
#endif
		}
		W += gw * (uint32_t)m;
		us = ue;
		vs = ve;
	}
}

static void span_tex_z_scalar(uint16_t *restrict dst,
                              uint16_t *restrict z,
                              int32_t        n,
                              const uint32_t s[PL_N],
                              const plane_t  pl[PL_N],
                              const uint16_t *restrict tex)
{
	span_tex_z_impl(dst, z, n, s, pl, tex, 0, 0, NULL);
}

static void span_tex_z(uint16_t *restrict dst,
                       uint16_t *restrict z,
                       int32_t        n,
                       const uint32_t s[PL_N],
                       const plane_t  pl[PL_N],
                       const uint16_t *restrict tex)
{
	span_tex_z_impl(dst, z, n, s, pl, tex, 1, 0, NULL);
}

/*
 * A constant-w NOZ textured span (pl[PL_W].gx == 0: the ground, ~99.9 % of
 * its px): span_tex_z_impl()'s noz arithmetic with everything that depends
 * on w alone -- the perspective divide's persp_w() and the fog level, so the
 * palette row too -- computed once per span instead of per sub-span, and the
 * sub-span loop specialised per palette/plain texture. The same values in
 * the same order: tr_raster_selfcheck() holds it to the scalar span.
 */
static inline __attribute__((always_inline)) void tex_cw_run(uint16_t *restrict dst,
                                                             int32_t  n,
                                                             uint32_t U,
                                                             uint32_t V,
                                                             uint32_t gu,
                                                             uint32_t gv,
                                                             persp_t  pw,
                                                             const uint16_t *restrict tex,
                                                             const uint8_t *restrict idx,
                                                             const uint8_t *restrict pal,
                                                             const int fogged)
{
	int32_t us, vs, ue, ve, x = 0;

	persp_uv(U, V, pw, &us, &vs);
	for (; x + TR_TEX_SUB < n; x += TR_TEX_SUB) { /* not the last sub-span: k == TR_TEX_SUB */
		U += gu * (uint32_t)TR_TEX_SUB;
		V += gv * (uint32_t)TR_TEX_SUB;
		persp_uv(U, V, pw, &ue, &ve);

		uint32_t ua = (uint32_t)us << 3, va = (uint32_t)vs << 3;
		uint32_t du = (uint32_t)((ue - us) * 8 / TR_TEX_SUB),
		         dv = (uint32_t)((ve - vs) * 8 / TR_TEX_SUB);

		if (fogged) {
			tex_run_noz_fog(&dst[x], TR_TEX_SUB, ua, du, va, dv, idx, pal);
		} else {
			tex_run_noz(&dst[x], TR_TEX_SUB, ua, du, va, dv, tex);
		}
		us = ue;
		vs = ve;
	}
	if (x < n) { /* the last sub-span: m px, end sample at its own last pixel */
		int32_t m = n - x, k = m - 1;

		U += gu * (uint32_t)k;
		V += gv * (uint32_t)k;
		persp_uv(U, V, pw, &ue, &ve);

		uint32_t ua = (uint32_t)us << 3, va = (uint32_t)vs << 3;
		uint32_t du = k ? (uint32_t)((ue - us) * 8 / k) : 0,
		         dv = k ? (uint32_t)((ve - vs) * 8 / k) : 0;

		if (fogged) {
			tex_run_noz_fog(&dst[x], m, ua, du, va, dv, idx, pal);
		} else {
			tex_run_noz(&dst[x], m, ua, du, va, dv, tex);
		}
	}
}

/* TR_TRI_NOZ textured span: tr_raster_selfcheck() holds it to span_tex_z()
 * on a cleared z band. No SLP vectoriser: it would gather a sub-span's 8
 * texels on the stack and reload them into NEON for one store, where 8 STRH
 * straight to the band are cheaper. */
__attribute__((optimize("no-tree-slp-vectorize"))) static void
span_tex(uint16_t *restrict dst,
         int32_t        n,
         const uint32_t s[PL_N],
         const plane_t  pl[PL_N],
         const uint16_t *restrict tex,
         const tr_tex_fog_t *fog)
{
	if (pl[PL_W].gx != 0) {
		span_tex_z_impl(dst, NULL, n, s, pl, tex, 0, 1, fog);
		return;
	}

	uint32_t lv = fog ? fog_level(s[PL_W]) : 0u;
	persp_t  pw = persp_w(s[PL_W]);

	if (lv != 0 || tex == NULL) { /* NULL: an index-only texture, level 0 through pal too (r3d.h) */
		tex_cw_run(dst,
		           n,
		           s[PL_U],
		           s[PL_V],
		           pl[PL_U].gx,
		           pl[PL_V].gx,
		           pw,
		           tex,
		           fog->idx,
		           (const uint8_t *)(fog->pal + lv * fog->npal),
		           1);
	} else {
		tex_cw_run(dst, n, s[PL_U], s[PL_V], pl[PL_U].gx, pl[PL_V].gx, pw, tex, NULL, NULL, 0);
	}
}

/*
 * The per-row loop of raster_setup(), one copy per `kind` (a compile-time
 * constant at every call: 0 no z, flat -- tr_span_fill (the painter's path
 * and TR_TRI_NOZ flat); 1 flat; 2 Gouraud; 3 textured; 4 NOZ Gouraud; 5 NOZ
 * textured), so the plane loops unroll to exactly the planes the kind uses
 * and the span dispatch folds away. Per row: pure int32 add/compare per edge
 * (edge_dda_step()'s arithmetic, inline) -- zero divisions, zero int64.
 * cne != 0: the edge counts (cnr right bounds of cne) are constants too
 * (raster_rows_ne()); 0: read from su.
 */
static inline __attribute__((always_inline)) void raster_rows(uint16_t             *fb,
                                                              uint32_t              stride_px,
                                                              int32_t               fb_y0,
                                                              const tr_tri_setup_t *su,
                                                              const edge_dda_t     *e,
                                                              const plane_t        *pl,
                                                              int32_t               ys,
                                                              int32_t               ye,
                                                              int32_t               y_lo,
                                                              uint16_t             *zband,
                                                              uint16_t              c,
                                                              const uint16_t       *tex,
                                                              const tr_tex_fog_t   *fog,
                                                              const int             kind,
                                                              const int             cnr,
                                                              const int             cne)
{
	const int32_t np = (kind == 3 || kind == 5)   ? PL_V + 1
	                   : (kind == 2 || kind == 4) ? PL_B + 1
	                                              : kind;
	const int32_t ne = cne ? cne : su->ne, nr = cne ? cnr : su->nr, ax = su->ax;
	const int     pk = kind == 5   ? TR_PROF_NOZ_TEX
	                   : kind == 3 ? TR_PROF_TEX
	                   : kind == 2 ? TR_PROF_GOURAUD
	                   : kind == 1 ? TR_PROF_FLAT
	                               : TR_PROF_NOZ_FILL;
	/* The DDAs and plane rows as locals: with the edge counts constant
	 * (cne != 0) the loops below unroll and they live in registers. */
	int32_t  q[3], rm[3], sq[3], sr[3], dn[3];
	uint32_t row[PL_N];

	(void)pk;
	for (int32_t i = 0; i < ne; i++) {
		q[i] = e[i].quot, rm[i] = e[i].rem, sq[i] = e[i].step_quot, sr[i] = e[i].step_rem,
		dn[i] = e[i].denom;
	}
	for (int32_t k = 0; k < np; k++) {
		row[k] = pl[k].row;
	}
	for (int32_t py = ys; py < ye; py++) {
		int32_t lo      = 0;        /* screen-clipped left bound so far */
		int32_t hi_excl = TR_R3D_W; /* screen-clipped right bound so far */

		/* e[0..nr) right bounds, e[nr..ne) left; each then steps one row
		 * (edge_dda_step()). */
		for (int32_t i = 0; i < ne; i++) {
			if (i < nr) {
				hi_excl = q[i] + 1 < hi_excl ? q[i] + 1 : hi_excl;
			} else {
				lo = q[i] > lo ? q[i] : lo;
			}
			q[i] += sq[i];
			rm[i] += sr[i];
			if (rm[i] >= dn[i]) {
				rm[i] -= dn[i];
				q[i] += 1;
			}
		}

		if (lo < hi_excl) {
			uint16_t *dst = &fb[(uint32_t)(py - fb_y0) * stride_px + (uint32_t)lo];
			uint16_t *zr  = kind >= 1 && kind <= 3
			                    ? &zband[(uint32_t)(py - y_lo) * TR_R3D_W + (uint32_t)lo]
			                    : NULL;
			uint32_t  sv[PL_N];
			int32_t   n = hi_excl - lo;

			for (int32_t k = 0; k < np; k++) {
				sv[k] = row[k] + pl[k].gx * (uint32_t)(lo - ax);
			}
			PROF_T0();
			if (kind == 5) {
				span_tex(dst, n, sv, pl, tex, fog);
			} else if (kind == 4) {
				span_gouraud(dst, n, sv, pl, TR_R3D_W - lo);
			} else if (kind == 3) {
				span_tex_z(dst, zr, n, sv, pl, tex);
			} else if (kind == 2) {
				span_gouraud_z(dst, zr, n, sv, pl, TR_R3D_W - lo);
			} else if (kind == 1) {
				span_flat_z(dst, zr, n, sv[PL_W], pl[PL_W].gx, c, TR_R3D_W - lo);
			} else {
				tr_span_fill(dst, (uint32_t)n, c);
			}
			PROF_ADD(pk, (uint32_t)n, 1);
		}
		for (int32_t k = 0; k < np; k++) {
			row[k] += pl[k].gy;
		}
	}
}

/* raster_rows() with the edge counts as constants: (nr, ne) is (1, 2) (one
 * horizontal edge), (1, 3) or (2, 3) for every drawable triangle. One copy
 * per case for the kinds that carry most rows (Gouraud ~85 % of them in the
 * scene, flat, NOZ Gouraud); textured spans keep the generic loop (large
 * span code, few rows). Silicon (a32/payload-isa, 2026-09-23, -O3
 * -funroll-loops): the edge walk 107.2 -> 75.6 cyc/row. */
static inline __attribute__((always_inline)) void raster_rows_ne(uint16_t             *fb,
                                                                 uint32_t              stride_px,
                                                                 int32_t               fb_y0,
                                                                 const tr_tri_setup_t *su,
                                                                 const edge_dda_t     *e,
                                                                 const plane_t        *pl,
                                                                 int32_t               ys,
                                                                 int32_t               ye,
                                                                 int32_t               y_lo,
                                                                 uint16_t             *zband,
                                                                 uint16_t              c,
                                                                 const uint16_t       *tex,
                                                                 const tr_tex_fog_t   *fog,
                                                                 const int             kind)
{
	if (su->ne == 2) {
		raster_rows(fb, stride_px, fb_y0, su, e, pl, ys, ye, y_lo, zband, c, tex, fog, kind, 1, 2);
	} else if (su->nr == 1) {
		raster_rows(fb, stride_px, fb_y0, su, e, pl, ys, ye, y_lo, zband, c, tex, fog, kind, 1, 3);
	} else {
		raster_rows(fb, stride_px, fb_y0, su, e, pl, ys, ye, y_lo, zband, c, tex, fog, kind, 2, 3);
	}
}

/*
 * Draws rows [max(y0, y_lo), min(y1, y_hi)) of one set-up triangle into
 * `fb`, whose first row is screen row fb_y0 (0: a framebuffer; y_lo: a
 * band buffer). The
 * setup is copied and advanced from its row y0 to the first row drawn by
 * int32 adds (edge_dda_skip(), one multiply per plane) -- no divide here.
 * zband == NULL is the painter's path (flat `c`, tr_span_fill, no z).
 */
static void raster_setup(uint16_t             *fb,
                         uint32_t              stride_px,
                         int32_t               fb_y0,
                         const tr_tri_t       *t,
                         const tr_tri_setup_t *su,
                         int32_t               y_lo,
                         int32_t               y_hi,
                         uint16_t             *zband)
{
	int32_t ys = su->y0 > y_lo ? su->y0 : y_lo;
	int32_t ye = su->y1 < y_hi ? su->y1 : y_hi;

	if (ys >= ye) {
		return;
	}

	int32_t             ne = su->ne;
	edge_dda_t          e[3];
	plane_t             pl[PL_N];
	const uint16_t      c     = t->c;
	const uint8_t       flags = zband ? t->flags : 0;
	const uint16_t     *tex   = tr_r3d_tex[t->tex & (TR_TEX_MAX - 1)];
	const int           noz   = (flags & TR_TRI_NOZ) != 0;
	const tr_tex_fog_t *fog   = tr_r3d_tex_fog[t->tex & (TR_TEX_MAX - 1)].idx
	                                ? &tr_r3d_tex_fog[t->tex & (TR_TEX_MAX - 1)]
	                                : NULL;
	int32_t             np    = (flags & TR_TRI_TEX)       ? PL_V + 1
	                            : (flags & TR_TRI_GOURAUD) ? PL_B + 1
	                            : (!zband || noz)          ? 0
	                                                       : 1;

	if ((flags & TR_TRI_TEX) && tex == NULL && !(noz && fog != NULL)) {
		return; /* no texture (an index-only one draws NOZ through its fog palette, r3d.h) */
	}
	for (int32_t i = 0; i < ne; i++) {
		e[i] = su->e[i];
		edge_dda_skip(&e[i], ys - su->y0);
	}
	for (int32_t k = 0; k < np; k++) {
		pl[k] = su->pl[k];
		pl[k].row += pl[k].gy * (uint32_t)(ys - su->y0);
	}
	PROF_T0();
	if (flags & TR_TRI_TEX) {
		if (noz) {
			raster_rows(fb, stride_px, fb_y0, su, e, pl, ys, ye, y_lo, zband, c, tex, fog, 5, 0, 0);
		} else {
			raster_rows(fb, stride_px, fb_y0, su, e, pl, ys, ye, y_lo, zband, c, tex, fog, 3, 0, 0);
		}
	} else if (flags & TR_TRI_GOURAUD) {
		if (noz) {
			raster_rows_ne(fb, stride_px, fb_y0, su, e, pl, ys, ye, y_lo, zband, c, tex, fog, 4);
		} else {
			raster_rows_ne(fb, stride_px, fb_y0, su, e, pl, ys, ye, y_lo, zband, c, tex, fog, 2);
		}
	} else if (np == 1) {
		raster_rows_ne(fb, stride_px, fb_y0, su, e, pl, ys, ye, y_lo, zband, c, tex, fog, 1);
	} else {
		raster_rows(fb, stride_px, fb_y0, su, e, pl, ys, ye, y_lo, zband, c, tex, fog, 0, 0, 0);
	}
	PROF_ADD(TR_PROF_TRI, (uint32_t)(ye - ys), 1);
}

void tr_raster_tri(uint16_t *fb, uint32_t stride_px, const tr_tri_t *t)
{
	tr_tri_setup_t su;

	if (tri_setup(&su, t, false)) {
		raster_setup(fb, stride_px, 0, t, &su, 0, TR_R3D_H, NULL);
	}
}

/* Lerp two RGB565 colours by t in [0,1] (Q8 fixed, 0..256) -- widen each
 * channel to 8 bits first, same reasoning as r3d_math.c's shade(): lerping
 * in the packed 5/6/5 domain rounds two of the three channels against the
 * wrong bit depth. */
static uint16_t lerp565(uint16_t a, uint16_t b, uint32_t t_q8)
{
	int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
	int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
	int ar8 = (ar << 3) | (ar >> 2), ag8 = (ag << 2) | (ag >> 4), ab8 = (ab << 3) | (ab >> 2);
	int br8 = (br << 3) | (br >> 2), bg8 = (bg << 2) | (bg >> 4), bb8 = (bb << 3) | (bb >> 2);

	/* |diff| * t_q8 <= 255 * 256: int32 is exact, no 64-bit divide. */
	int r8 = ar8 + (br8 - ar8) * (int)t_q8 / 256;
	int g8 = ag8 + (bg8 - ag8) * (int)t_q8 / 256;
	int b8 = ab8 + (bb8 - ab8) * (int)t_q8 / 256;

	return (uint16_t)(((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b8 >> 3));
}

/* Sky colour of row y for a gradient over rows [0, rows), rows >= 1. The
 * rows == 1 case would divide by zero, hence the guard; 32-bit divide. */
static uint16_t sky_row(int32_t y, int32_t rows, uint16_t top, uint16_t bot)
{
	uint32_t t_q8 = (rows > 1) ? (uint32_t)y * 256u / (uint32_t)(rows - 1) : 0;

	return lerp565(top, bot, t_q8);
}

void tr_r3d_sky(uint16_t *fb, uint32_t stride_px, int32_t horizon_y, uint16_t top, uint16_t bot)
{
	int32_t rows = clampi(horizon_y, 0, TR_R3D_H);

	for (int32_t y = 0; y < rows; y++) {
		tr_span_fill(&fb[(uint32_t)y * stride_px], TR_R3D_W, sky_row(y, rows, top, bot));
	}
}

/* 4x4 Bayer matrix: the ordered-dither threshold of pixel (x, y) is
 * (2 * M[y & 3][x & 3] + 1) / 32 of one RGB565 step. */
static const uint8_t bayer4[4][4] = { { 0, 8, 2, 10 },
	                                  { 12, 4, 14, 6 },
	                                  { 3, 11, 1, 9 },
	                                  { 15, 7, 13, 5 } };

/* RGB565 channels of c, << 8 (channel units, 8 fraction bits). */
static void ch_q8(uint16_t c, int32_t v[3])
{
	v[0] = (int32_t)(c >> 11) << 8;
	v[1] = (int32_t)((c >> 5) & 0x3F) << 8;
	v[2] = (int32_t)(c & 0x1F) << 8;
}

/* The dithered sky's channels at row y of `rows` (>= 1), channel units Q8. */
static void sky_q8(const tr_bg_t *bg, int32_t y, int32_t rows, int32_t v[3])
{
	uint32_t t = rows > 1 ? (uint32_t)y * 65536u / (uint32_t)(rows - 1) : 0; /* Q16 */
	uint16_t a = bg->top, b = bg->bot;
	int32_t  ca[3], cb[3];

	if (bg->mid_q8 != 0) {
		uint32_t m = (uint32_t)bg->mid_q8 << 8;

		if (t < m) {
			b = bg->mid;
			t = (t << 8) / bg->mid_q8;
		} else {
			a = bg->mid;
			t = ((t - m) << 8) / (256u - bg->mid_q8);
		}
	}
	ch_q8(a, ca);
	ch_q8(b, cb);
	for (int k = 0; k < 3; k++) {
		v[k] = ca[k] + (int32_t)(((int64_t)(cb[k] - ca[k]) * (int32_t)t) >>
		                         16); /* 32x32 SMULL, no divide */
	}
}

/* Dithered RGB565 of channels v (Q8, each <= its max << 8) at threshold d. */
static inline uint16_t dither565(const int32_t v[3], int32_t d)
{
	return (uint16_t)(((v[0] + d) >> 8) << 11 | ((v[1] + d) >> 8) << 5 | ((v[2] + d) >> 8));
}

/* px pixels (a multiple of 8) of the 4-px pattern p[0..3], starting at pattern
 * phase 0: rows of TR_R3D_W (% 8 == 0) back to back keep the phase. NEON:
 * VSTMIA of 64 B when dst is word-aligned (the renderer's SRAM band buffers
 * are 64 B aligned; a host test's static band -- e.g. render.c's band_mem in
 * the golden/host build -- may be only 2-byte aligned and takes the VST1Q
 * loop throughout), then 16 B stores -- silicon (a32/payload-isa, 2026-09-23, band-sized buffer):
 * VSTM 0.105 cyc/B against 0.188 for VST1Q and 0.240 for newlib memset. */
_Static_assert(TR_R3D_W % 8 == 0, "fill_pattern(): whole rows are whole 16-byte stores");

static void fill_pattern(uint16_t *dst, uint32_t px, const uint16_t p[4])
{
#if defined(__ARM_NEON)
	uint16x4_t h = vld1_u16(p);
	uint16x8_t v = vcombine_u16(h, h);
	uint8_t   *d = (uint8_t *)dst;
	uint32_t   n = px * 2u;

	if (n >= 64u && ((uintptr_t)d & 3u) == 0) {
		uint32_t blk = n & ~63u;

		__asm__ volatile("vmov q8, %q2\n\tvmov q9, %q2\n\tvmov q10, %q2\n\tvmov q11, %q2\n"
		                 "1:\tvstmia %0!, {d16-d23}\n\tsubs %1, %1, #64\n\tbne 1b"
		                 : "+r"(d), "+r"(blk)
		                 : "w"(v)
		                 : "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23", "cc", "memory");
		n &= 63u;
	}
	for (; n != 0; n -= 16u, d += 16) {
		vst1q_u16((uint16_t *)(void *)d, v);
	}
#else
	for (uint32_t x = 0; x < px; x++) {
		dst[x] = p[x & 3];
	}
#endif
}

/* Floor square root of n < 2^31 (per-row halo width; setup-rate). */
static int32_t isqrt(uint32_t n)
{
	uint32_t r = 0, b = 1u << 30;

	while (b > n) {
		b >>= 2;
	}
	for (; b != 0; b >>= 2) {
		if (n >= r + b) {
			n -= r + b;
			r = (r >> 1) + b;
		} else {
			r >>= 1;
		}
	}
	return (int32_t)r;
}

/* 2^(-f/64) in Q15, f = 0..63: exp2q15(i) = 2^(-i/64) = this >> (i / 64). */
static const uint16_t exp2_frac[64] = {
	32767, 32414, 32065, 31719, 31378, 31040, 30705, 30375, 30047, 29724, 29404, 29087, 28774,
	28464, 28157, 27854, 27554, 27257, 26963, 26673, 26385, 26101, 25820, 25542, 25267, 24995,
	24725, 24459, 24196, 23935, 23677, 23422, 23170, 22920, 22673, 22429, 22187, 21948, 21712,
	21478, 21247, 21018, 20791, 20568, 20346, 20127, 19910, 19696, 19483, 19274, 19066, 18861,
	18657, 18456, 18258, 18061, 17866, 17674, 17483, 17295, 17109, 16925, 16742, 16562,
};

static inline int32_t exp2q15(uint32_t i)
{
	return i >= 64u * 15u ? 0 : exp2_frac[i & 63u] >> (i >> 6);
}

/* Q15 x Q15, rounded: exactly NEON VQRDMULH for operands in [0, 32767]. */
static inline int32_t mul15(int32_t a, int32_t b)
{
	return (2 * a * b + 32768) >> 16;
}

/*
 * The halo is two Gaussians of d / halo_r -- a wide glow exp(-4 u) and a hot
 * core exp(-40 u), u = d^2 / halo_r^2 -- and a Gaussian is separable:
 * exp(-k (dx^2 + dy^2)) = exp(-k dx^2) exp(-k dy^2). So a band tabulates the
 * column factors once (halo_cols()), a row its two row factors, and a pixel is
 * two multiplies: I = col_g * row_g + col_c * row_c (Q15, amplitudes 0.55 and
 * 0.45 folded into the rows). HALO_KG / HALO_KC are 5 and 40 as base-2
 * exponents in 1/64 units (4 / ln 2 * 64, 40 / ln 2 * 64).
 */
#define HALO_KG 369u
#define HALO_KC 3693u
#define HALO_AG 18022 /* 0.55 Q15 */
#define HALO_AC 14745 /* 0.45 Q15 */

typedef struct {
	int32_t  x0, x1;                   /* columns tabulated, multiples of 4 */
	int16_t  g[TR_R3D_W], c[TR_R3D_W]; /* Q15 column factors */
	uint32_t mg, mc;                   /* exponent per px^2, << 16 */
} halo_cols_t;

/* Base-2 exponent (1/64 units) of dx px at slope m (<< 16): dx <= halo_r + 3,
 * so dx^2 * m <= (1 + 3 / 8)^2 * HALO_KC * 2^16 < 2^32 for halo_r >= 8. */
static inline uint32_t halo_arg(int32_t dx, uint32_t m)
{
	return ((uint32_t)(dx * dx) * m) >> 16;
}

static void halo_cols(halo_cols_t *hc, const tr_bg_t *bg)
{
	int32_t  r  = bg->halo_r;
	uint32_t r2 = (uint32_t)(r * r);

	hc->mg = (HALO_KG << 16) / r2;
	hc->mc = (HALO_KC << 16) / r2;
	hc->x0 = (bg->sun_x - r) & ~3;
	hc->x1 = (bg->sun_x + r + 4) & ~3;
	hc->x0 = hc->x0 < 0 ? 0 : hc->x0;
	hc->x1 = hc->x1 > TR_R3D_W ? TR_R3D_W : hc->x1;
	for (int32_t x = hc->x0; x < hc->x1; x++) {
		hc->g[x] = (int16_t)exp2q15(halo_arg(x - bg->sun_x, hc->mg));
		hc->c[x] = (int16_t)exp2q15(halo_arg(x - bg->sun_x, hc->mc));
	}
}

/* Pixels [x, x + n) of a halo row, scalar: the reference for the NEON form. */
static void halo_px_scalar(uint16_t          *dst,
                           const halo_cols_t *hc,
                           int32_t            x,
                           int32_t            n,
                           int32_t            rg,
                           int32_t            rc,
                           const int32_t      v[3],
                           const int32_t      h[3],
                           const uint8_t     *bm)
{
	for (int32_t i = x; i < x + n; i++) {
		int32_t in = (mul15(hc->g[i], rg) + mul15(hc->c[i], rc)) >> 7; /* 0..255 */
		int32_t d  = 16 * bm[i & 3] + 8;

		dst[i] = (uint16_t)(((v[0] + in * h[0] + d) >> 8) << 11 |
		                    ((v[1] + in * h[1] + d) >> 8) << 5 | ((v[2] + in * h[2] + d) >> 8));
	}
}

/* The halo over sky row y (channels v, Q8): columns within halo_r of the
 * centre (rounded out to multiples of 4, so an 8-px chunk starts on dither
 * phase 0), re-dithered with the halo added. */
static void halo_row(uint16_t          *dst,
                     const tr_bg_t     *bg,
                     const halo_cols_t *hc,
                     int32_t            y,
                     const int32_t      v[3],
                     int                vec)
{
	int32_t r = bg->halo_r, dy = y - bg->sun_y;

	if (dy <= -r || dy >= r) {
		return;
	}
	int32_t hw = isqrt((uint32_t)(r * r - dy * dy));
	int32_t x0 = (bg->sun_x - hw) & ~3, x1 = (bg->sun_x + hw + 4) & ~3;

	x0 = x0 < hc->x0 ? hc->x0 : x0;
	x1 = x1 > hc->x1 ? hc->x1 : x1;
	if (x0 >= x1) {
		return;
	}

	/* Halo channels, capped so v + 255 h never passes the channel max. */
	static const int32_t mx[3] = { 31 << 8, 63 << 8, 31 << 8 };
	int32_t              h[3];
	int32_t              rg = mul15(exp2q15(halo_arg(dy, hc->mg)), HALO_AG);
	int32_t              rc = mul15(exp2q15(halo_arg(dy, hc->mc)), HALO_AC);
	const uint8_t       *bm = bayer4[y & 3];

	ch_q8(bg->halo, h);
	for (int k = 0; k < 3; k++) {
		int32_t cap = (mx[k] - v[k]) >> 8;

		h[k] = (h[k] >> 8) < cap ? (h[k] >> 8) : cap;
	}
	(void)vec;
#if defined(__ARM_NEON)
	if (vec && x1 - x0 >= 8) {
		uint16_t bd[3][8];

		for (int k = 0; k < 8; k++) {
			for (int ch = 0; ch < 3; ch++) {
				bd[ch][k] = (uint16_t)(v[ch] + 16 * bm[k & 3] + 8);
			}
		}
		const uint16x8_t br = vld1q_u16(bd[0]), bgv = vld1q_u16(bd[1]), bb = vld1q_u16(bd[2]);
		const int16x8_t  vrg = vdupq_n_s16((int16_t)rg), vrc = vdupq_n_s16((int16_t)rc);
		const uint16x8_t hr = vdupq_n_u16((uint16_t)h[0]), hg = vdupq_n_u16((uint16_t)h[1]);
		const uint16x8_t hb = vdupq_n_u16((uint16_t)h[2]);

		for (int32_t x = x0;;) {
			int16x8_t  ig = vqrdmulhq_s16(vld1q_s16(&hc->g[x]), vrg);
			int16x8_t  ic = vqrdmulhq_s16(vld1q_s16(&hc->c[x]), vrc);
			uint16x8_t in = vshrq_n_u16(vreinterpretq_u16_s16(vaddq_s16(ig, ic)), 7);
			uint16x8_t cr = vshrq_n_u16(vmlaq_u16(br, in, hr), 8);
			uint16x8_t cg = vshrq_n_u16(vmlaq_u16(bgv, in, hg), 8);
			uint16x8_t cb = vshrq_n_u16(vmlaq_u16(bb, in, hb), 8);

			vst1q_u16(&dst[x], vsliq_n_u16(vsliq_n_u16(cb, cg, 5), cr, 11));
			x += 8;
			if (x >= x1) {
				return;
			}
			x = x + 8 > x1 ? x1 - 8 : x; /* ragged end: x1 - 8 is phase 0 too */
		}
	}
#endif
	halo_px_scalar(dst, hc, x0, x1 - x0, rg, rc, v, h, bm);
}

/* Star i: hashed x, height above the horizon, brightness. */
static uint32_t star_hash(uint32_t i)
{
	i = (i + 0x9E3779B9u) * 0x85EBCA6Bu;
	i ^= i >> 13;
	i *= 0xC2B2AE35u;
	return i ^ (i >> 16);
}

/* a (0..256) of the way from dst toward c, per channel. */
static uint16_t blend565(uint16_t dst, uint16_t c, uint32_t a)
{
	uint32_t r = ((dst >> 11) * (256u - a) + (uint32_t)(c >> 11) * a) >> 8;
	uint32_t g = (((dst >> 5) & 63u) * (256u - a) + ((uint32_t)(c >> 5) & 63u) * a) >> 8;
	uint32_t b = ((dst & 31u) * (256u - a) + ((uint32_t)c & 31u) * a) >> 8;

	return (uint16_t)(r << 11 | g << 5 | b);
}

/* Stars over sky rows [y_lo, min(y_hi, horizon)) of the band. Height above
 * the horizon 16..527 px; they fade in from 60 px up to full at 420 px, so
 * the glow near the horizon stays clean. The brightest ~17 % get a 4-px cross
 * at half strength. */
static void band_stars(uint16_t *cband, int32_t y_lo, int32_t y_hi, int32_t rows, uint32_t dim)
{
	int32_t ye = y_hi < rows ? y_hi : rows;

	for (uint32_t i = 0; i < TR_BG_STAR_N; i++) {
		uint32_t h  = star_hash(i);
		int32_t  ht = 16 + (int32_t)((h >> 16) & 511u);
		int32_t  y  = rows - ht;

		if (y < y_lo - 1 || y > ye) { /* +-1 row: a cross's arms */
			continue;
		}
		int32_t  x    = (int32_t)(((h & 0xFFFFu) * (uint32_t)(TR_R3D_W - 2)) >> 16) + 1;
		uint32_t b    = 96u + ((h >> 26) << 2) + ((h >> 8) & 3u) * 8u; /* 96..372 */
		int32_t  fade = ht < 60 ? 0 : ht > 420 ? 360 : ht - 60;
		uint32_t a    = (b * (uint32_t)fade) / 360u;
		uint16_t c =
		    (h & 0x300u) == 0x300u ? 0xFF16u : 0xDF7Fu; /* a quarter warm, the rest blue-white */

		a = a > 256u ? 256u : a;
		a = (a * (256u - dim)) >> 8;
		if (a == 0) {
			continue;
		}
		if (y >= y_lo && y < ye) {
			uint16_t *p = &cband[(uint32_t)(y - y_lo) * TR_R3D_W + (uint32_t)x];

			*p = blend565(*p, c, a);
			if (b > 330u) {
				p[-1] = blend565(p[-1], c, a / 2);
				p[1]  = blend565(p[1], c, a / 2);
			}
		}
		if (b > 330u) {
			for (int32_t yy = y - 1; yy <= y + 1; yy += 2) {
				if (yy >= y_lo && yy < ye) {
					uint16_t *p = &cband[(uint32_t)(yy - y_lo) * TR_R3D_W + (uint32_t)x];

					*p = blend565(*p, c, a / 2);
				}
			}
		}
	}
}

/* Background rows [y_lo, y_hi) into the band buffer: the sky above
 * bg->horizon (tr_bg_t), bg->ground below. */
static void band_background(uint16_t *cband, int32_t y_lo, int32_t y_hi, const tr_bg_t *bg)
{
	int32_t     rows = clampi(bg->horizon, 0, TR_R3D_H);
	halo_cols_t hc;
	int         halo = (bg->fx & TR_BG_DITHER) && bg->halo_r >= 8 && y_lo < rows &&
	                   y_lo < bg->sun_y + bg->halo_r && y_hi > bg->sun_y - bg->halo_r;

	if (halo) {
		halo_cols(&hc, bg);
		halo = hc.x0 < hc.x1;
	}
	int32_t ys = y_hi < rows ? y_hi : rows; /* sky rows [y_lo, ys), ground rows [ys, y_hi) */

	if (ys < y_lo) {
		ys = y_lo;
	}
	if (ys < y_hi) {
		const uint16_t g[4] = { bg->ground, bg->ground, bg->ground, bg->ground };

		fill_pattern(&cband[(uint32_t)(ys - y_lo) * TR_R3D_W], (uint32_t)(y_hi - ys) * TR_R3D_W, g);
	}
	for (int32_t y = y_lo; y < ys; y++) {
		uint16_t *dst = &cband[(uint32_t)(y - y_lo) * TR_R3D_W];

		if (!(bg->fx & TR_BG_DITHER)) {
			uint16_t c    = sky_row(y, rows, bg->top, bg->bot);
			uint16_t p[4] = { c, c, c, c };

			fill_pattern(dst, TR_R3D_W, p);
		} else {
			int32_t  v[3];
			uint16_t p[4];

			sky_q8(bg, y, rows, v);
			for (int k = 0; k < 4; k++) {
				p[k] = dither565(v, 2 * bayer4[y & 3][k] * 8 + 8);
			}
			fill_pattern(dst, TR_R3D_W, p);
			if (halo) {
				halo_row(dst, bg, &hc, y, v, 1);
			}
		}
	}
	if ((bg->fx & TR_BG_DITHER) && (bg->fx & TR_BG_STARS) && y_lo < rows) {
		band_stars(cband, y_lo, y_hi, rows, bg->star_dim);
	}
}

/* Finished band -> framebuffer: full rows, write-only to the FB (the FB is
 * Normal non-cacheable on the A32; a read there is a ~97 MB/s bus round
 * trip). NEON: 8-px loads from the cached band, 8-px stores to the FB. */
static void
band_copy(uint16_t *fb, uint32_t stride_px, const uint16_t *cband, int32_t y_lo, int32_t y_hi)
{
	for (int32_t y = y_lo; y < y_hi; y++) {
		uint16_t *restrict d       = &fb[(uint32_t)y * stride_px];
		const uint16_t *restrict s = &cband[(uint32_t)(y - y_lo) * TR_R3D_W];

#if defined(__ARM_NEON)
		for (int32_t x = 0; x < TR_R3D_W; x += 8) { /* TR_R3D_W % 8 == 0 */
			vst1q_u16(&d[x], vld1q_u16(&s[x]));
		}
#else
		memcpy(d, s, TR_R3D_W * sizeof(*d));
#endif
	}
}

void tr_raster_band(uint16_t             *fb,
                    uint32_t              stride_px,
                    int                   y_lo,
                    int                   y_hi,
                    uint16_t             *zband,
                    uint16_t             *cband,
                    const tr_bg_t        *bg,
                    const tr_dl_t        *dl,
                    const tr_tri_setup_t *setup,
                    const uint16_t       *bin,
                    uint32_t              nbin)
{
	PROF_T0();
	static const uint16_t zero[4];

	fill_pattern(zband, (uint32_t)(y_hi - y_lo) * TR_R3D_W, zero);
	band_background(cband, y_lo, y_hi, bg);
	PROF_ADD(TR_PROF_BG, (uint32_t)(y_hi - y_lo) * TR_R3D_W, 1);
	for (uint32_t i = 0; i < nbin; i++) {
		raster_setup(cband, TR_R3D_W, y_lo, &dl->tri[bin[i]], &setup[bin[i]], y_lo, y_hi, zband);
	}
	if (fb != NULL) {
		band_copy(fb, stride_px, cband, y_lo, y_hi);
	}
}

void tr_tri_setup_range(const tr_dl_t *dl, tr_tri_setup_t *setup, uint32_t lo, uint32_t hi)
{
	PROF_T0();
	for (uint32_t i = lo; i < hi; i++) {
		(void)tri_setup(&setup[i], &dl->tri[i], true); /* false leaves y0 == y1: binned nowhere */
	}
	PROF_ADD(TR_PROF_SETUP, 0, hi - lo);
}

void tr_bin_only(const tr_dl_t        *dl,
                 const tr_tri_setup_t *setup,
                 uint16_t              bins[TR_BANDS][TR_BIN_MAX],
                 uint32_t              counts[TR_BANDS],
                 uint32_t             *overflow)
{
	PROF_T0();
	memset(counts, 0, TR_BANDS * sizeof(counts[0]));
	/* Pass 0 bins the TR_TRI_NOZ background, pass 1 the rest: a band draws
	 * its ground before anything z-tests against the cleared z. */
	for (uint32_t pass = 0; pass < 2; pass++) {
		for (uint16_t i = 0; i < dl->n; i++) {
			if (setup[i].y0 >= setup[i].y1 || ((dl->tri[i].flags & TR_TRI_NOZ) ? 0u : 1u) != pass) {
				continue;
			}
			for (int32_t b = setup[i].y0 >> TR_BAND_SHIFT; b <= (setup[i].y1 - 1) >> TR_BAND_SHIFT;
			     b++) {
				if (counts[b] < TR_BIN_MAX) {
					bins[b][counts[b]++] = i;
				} else {
					(*overflow)++;
				}
			}
		}
	}
	PROF_ADD(TR_PROF_BIN, 0, dl->n);
}

void tr_bin_build(const tr_dl_t  *dl,
                  tr_tri_setup_t *setup,
                  uint16_t        bins[TR_BANDS][TR_BIN_MAX],
                  uint32_t        counts[TR_BANDS],
                  uint32_t       *overflow)
{
	tr_tri_setup_range(dl, setup, 0, dl->n);
	tr_bin_only(dl, setup, bins, counts, overflow);
}

int tr_raster_selfcheck(void)
{
	uint16_t d0[48], d1[48], z0[48], z1[48];
	uint32_t rs = 0x2468ACE1u;

	static uint16_t tex[TR_TEX_DIM * TR_TEX_DIM];

	for (uint32_t i = 0; i < TR_TEX_DIM * TR_TEX_DIM; i++) {
		tex[i] = (uint16_t)(i * 2654435761u >> 13);
	}
	/* kind 0..2: z-tested flat/Gouraud/textured, NEON vs scalar; 3, 4: NOZ
	 * Gouraud/textured vs the scalar z-tested span on a cleared z band
	 * (w >= 1 there, so every pixel passes: the colours must match). */
	for (int kind = 0; kind < 5; kind++) {
		for (int32_t off = 0; off < 8; off++) {
			for (int32_t n = 0; n <= 40; n++) {
				uint32_t sv[PL_N];
				plane_t  pl[PL_N];

				for (int k = 0; k < PL_N; k++) {
					rs ^= rs << 13, rs ^= rs >> 17, rs ^= rs << 5;
					sv[k] = rs;
					rs ^= rs << 13, rs ^= rs >> 17, rs ^= rs << 5;
					pl[k].gx  = rs >> (k == PL_W ? 8 : 12); /* w wraps within a span */
					pl[k].row = pl[k].gy = 0;
				}
				if (kind >= 2) { /* w >= 1/4 of near, u/v in a real range */
					sv[PL_W] = 0x40000000u | (sv[PL_W] & 0x3FFFFFFFu);
					pl[PL_W].gx &= 0xFFFFu;
					sv[PL_U] &= 0xFFFFFFu, sv[PL_V] &= 0xFFFFFFu;
					pl[PL_U].gx &= 0xFFFFu, pl[PL_V].gx &= 0xFFFFu;
					if (n & 1) {
						pl[PL_W].gx = 0; /* constant w: span_tex()'s hoisted copy */
					}
				}
				for (int i = 0; i < 48; i++) {
					rs ^= rs << 13, rs ^= rs >> 17, rs ^= rs << 5;
					d0[i] = d1[i] = (uint16_t)rs;
					z0[i] = z1[i] = (uint16_t)(rs >> 16);
				}
				/* Room to the row end: n (the span ends the row: overlapped
				 * end) .. the buffer's end (masked end, lanes past n kept). */
				int32_t room = n + (int32_t)(rs % (uint32_t)(48 - off - n + 1));
				(void)room; /* scalar build: the span macros drop it */

				if (kind == 0) {
					span_flat_z(&d0[off], &z0[off], n, sv[PL_W], pl[PL_W].gx, 0x5A5Au, room);
					span_flat_z_scalar(&d1[off], &z1[off], n, sv[PL_W], pl[PL_W].gx, 0x5A5Au);
				} else if (kind == 1) {
					span_gouraud_z(&d0[off], &z0[off], n, sv, pl, room);
					span_gouraud_z_scalar(&d1[off], &z1[off], n, sv, pl);
				} else if (kind == 2) {
					span_tex_z(&d0[off], &z0[off], n, sv, pl, tex);
					span_tex_z_scalar(&d1[off], &z1[off], n, sv, pl, tex);
				} else {
					memset(z1, 0, sizeof(z1));
					if (kind == 3) {
						span_gouraud(&d0[off], n, sv, pl, room);
						span_gouraud_z_scalar(&d1[off], &z1[off], n, sv, pl);
					} else {
						span_tex(&d0[off], n, sv, pl, tex, NULL);
						span_tex_z_scalar(&d1[off], &z1[off], n, sv, pl, tex);
					}
					memcpy(z1, z0, sizeof(z1)); /* NOZ: no z to compare */
				}
				if (memcmp(d0, d1, sizeof(d0)) != 0 || memcmp(z0, z1, sizeof(z0)) != 0) {
					return 0;
				}
			}
		}
	}
	/* recip62() against the divide: small a, around powers of two, random. */
	for (uint32_t i = 1; i < 1200u; i++) {
		uint64_t a;

		rs ^= rs << 13, rs ^= rs >> 17, rs ^= rs << 5;
		a = i < 400u   ? 1000u + i
		    : i < 800u ? ((uint64_t)1 << (10u + i % 42u)) + (i & 3u) - 2u
		               : (uint64_t)rs << (i % 21u) | 1u;
		if (recip62(a) != ((uint64_t)1 << 62) / a) {
			return 0;
		}
	}
	/* The halo row: NEON chunks vs the scalar reference, over centres, radii
	 * and rows that clip it at both screen edges and leave 4-px spans. */
	{
		static halo_cols_t   hc;
		static uint16_t      h0[TR_R3D_W], h1[TR_R3D_W];
		static const int16_t cfg[][3] = {
			{ 360, 287, 0 }, { 5, 40, 20 }, { 715, 100, 99 }, { 200, 9, 8 }, { -30, 64, 10 }
		};

		for (uint32_t k = 0; k < sizeof(cfg) / sizeof(cfg[0]); k++) {
			tr_bg_t bg = { 0 };

			bg.sun_x = cfg[k][0], bg.sun_y = 0, bg.halo_r = (uint16_t)cfg[k][1], bg.halo = 0xFFFFu;
			halo_cols(&hc, &bg);
			for (int32_t y = -cfg[k][1]; y <= cfg[k][1]; y += 3) {
				int32_t v[3] = { (int32_t)(rs & 0x1FFFu),
					             (int32_t)(rs >> 13 & 0x3FFFu),
					             (int32_t)(rs >> 3 & 0x1FFFu) };

				rs ^= rs << 13, rs ^= rs >> 17, rs ^= rs << 5;
				v[0] = v[0] > (31 << 8) ? 31 << 8 : v[0]; /* a channel never passes its max */
				v[1] = v[1] > (63 << 8) ? 63 << 8 : v[1];
				v[2] = v[2] > (31 << 8) ? 31 << 8 : v[2];
				memset(h0, 0, sizeof(h0));
				memset(h1, 0, sizeof(h1));
				halo_row(h0, &bg, &hc, y + cfg[k][2], v, 1);
				halo_row(h1, &bg, &hc, y + cfg[k][2], v, 0);
				if (memcmp(h0, h1, sizeof(h0)) != 0) {
					return 0;
				}
			}
		}
	}
	return 1;
}

void tr_r3d_draw(uint16_t *fb, uint32_t stride_px, const tr_dl_t *dl)
{
	for (uint16_t i = 0; i < dl->n; i++) {
		tr_raster_tri(fb, stride_px, &dl->tri[i]);
	}
}
