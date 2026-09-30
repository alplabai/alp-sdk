/* a32/payload-isa/isa_bench.c -- the portable half of the A32 ISA
 * microbenchmark (isa_bench.h). Includes src/render/r3d_raster.c itself, so
 * the "current" rows time the renderer's own static kernels (span_gouraud_z,
 * tri_setup, tex_run_noz, tr_raster_band), not copies of them.
 *
 * ISA_PAYLOAD=1 (the payload build): PMCCNTR timings, CP15 reads. Otherwise
 * (host cc, qemu-arm) timings read 0 and only the bit-exact checks mean
 * anything -- `make check` runs both and fails on any check failure.
 */
#include "../../src/render/r3d_raster.c"

#include "../../tests/host/tr_golden_dl.h"
#include "../common/crc32.h"
#include "isa_bench.h"

#ifndef ISA_PAYLOAD
#define ISA_PAYLOAD 0
#endif
#ifndef ISA_OPT
#define ISA_OPT 0
#endif

#define BAND_PX (TR_R3D_W * TR_BAND_H)

/* Unrolled in C, not with .rept: GCC sizes an asm by its lines, and a hidden
 * .rept pushed a literal pool out of VLDR range (-O3 -funroll-loops). */
#define R2(s)  s s
#define R4(s)  R2(s) R2(s)
#define R16(s) R4(R4(s))

#if ISA_PAYLOAD
static inline uint32_t cyc(void)
{
	uint32_t c;

	__asm__ volatile("isb\n\tmrc p15, 0, %0, c9, c13, 0" : "=r"(c)::"memory");
	return c;
}

/* What the renderer's tr_prof_now() is: no ISB. */
static inline uint32_t cyc_raw(void)
{
	uint32_t c;

	__asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(c)::"memory");
	return c;
}

static inline uint32_t mpidr(void)
{
	uint32_t v;

	__asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(v));
	return v;
}
#else
static inline uint32_t cyc(void)
{
	return 0;
}
static inline uint32_t cyc_raw(void)
{
	return 0;
}
static inline uint32_t mpidr(void)
{
	return 0;
}
#endif

static uint32_t rs; /* seeded by each stage that draws: .bss, not .data (isa.ld) */

static uint32_t rnd(void)
{
	rs ^= rs << 13, rs ^= rs >> 17, rs ^= rs << 5;
	return rs;
}

static void check(isa_res_t *r, int bit, int ok)
{
	if (ok) {
		r->pass |= 1u << bit;
	} else {
		r->fail |= 1u << bit;
	}
}

static void put(isa_res_t *r, int k, uint32_t c, uint32_t units)
{
	r->t[k].cyc   = c;
	r->t[k].units = units;
}

/* ---- PMU read cost and the renderer's profile hook (renderer.c) ---- */

static tr_prof_t hook_prof[2][TR_PROF_N] __attribute__((aligned(64)));

__attribute__((noinline)) static tr_prof_t *hook_core(void)
{
	return hook_prof[mpidr() & 1u];
}

__attribute__((noinline)) static uint32_t hook_now(void)
{
	return cyc_raw();
}

static void stage_pmu(isa_res_t *r)
{
	enum { N = 1024 };
	uint32_t t0 = cyc(), acc = 0;

	for (int i = 0; i < N / 8; i++) {
		acc += cyc_raw() + cyc_raw() + cyc_raw() + cyc_raw() + cyc_raw() + cyc_raw() + cyc_raw() +
		       cyc_raw();
	}
	put(r, ISA_PMU_READ, cyc() - t0, N);
	t0 = cyc();
	for (int i = 0; i < N; i++) { /* PROF_T0() + PROF_ADD() around an empty span */
		uint32_t   p0 = hook_now();
		tr_prof_t *p  = &hook_core()[TR_PROF_GOURAUD];

		p->cyc += hook_now() - p0;
		p->px += 9u;
		p->n += 1u;
	}
	put(r, ISA_PROF_HOOK, cyc() - t0, N);
	r->val[ISA_GZ_SPANS] += acc & 0u; /* keep acc live */
}

/* ---- FP divide: VDIV vs VRECPE + 2 x VRECPS ---- */

#if defined(__arm__) && defined(__ARM_NEON)
#include <arm_neon.h>

#define ITERS 256u /* x 16 ops */

static void stage_fdiv(isa_res_t *r)
{
	float    a = 3.0f, x = 1.5f, x1 = 1.5f, x2 = 1.5f, x3 = 1.5f;
	uint32_t t0 = cyc();

	for (uint32_t i = 0; i < ITERS; i++) {
		__asm__ volatile(R16("vdiv.f32 %0, %1, %0\n\t") : "+t"(x) : "t"(a));
	}
	put(r, ISA_FDIV_LAT, cyc() - t0, ITERS * 16u);
	check(r, ISA_FDIV_OK, x == 1.5f); /* 3/1.5 = 2, 3/2 = 1.5: an even count lands back */
	t0 = cyc();
	for (uint32_t i = 0; i < ITERS; i++) {
		__asm__ volatile(R4("vdiv.f32 %0, %4, %0\n\tvdiv.f32 %1, %4, %1\n\t"
		                    "vdiv.f32 %2, %4, %2\n\tvdiv.f32 %3, %4, %3\n\t")
		                 : "+t"(x), "+t"(x1), "+t"(x2), "+t"(x3)
		                 : "t"(a));
	}
	put(r, ISA_FDIV_TPUT, cyc() - t0, ITERS * 16u);

	/* One "divide" = recpe, 2 Newton steps, times a: 6 dependent ops. */
#define RECP(X, R, T, A) \
	"vrecpe.f32 " R ", " X "\n\tvrecps.f32 " T ", " X ", " R "\n\tvmul.f32 " R ", " R ", " T \
	"\n\t" \
	"vrecps.f32 " T ", " X ", " R "\n\tvmul.f32 " R ", " R ", " T "\n\tvmul.f32 " X ", " A ", " R \
	"\n\t"
	float32x2_t d = vdup_n_f32(1.5f), d1 = d, d2 = d, d3 = d, da = vdup_n_f32(3.0f), dr, dt, er, et,
	            fr, ft, gr, gt;

	t0 = cyc();
	for (uint32_t i = 0; i < ITERS; i++) {
		__asm__ volatile(R4(RECP("%P0", "%P1", "%P2", "%P3"))
		                 : "+w"(d), "=&w"(dr), "=&w"(dt)
		                 : "w"(da));
	}
	put(r, ISA_RECP_LAT, cyc() - t0, ITERS * 4u);
	t0 = cyc();
	for (uint32_t i = 0; i < ITERS; i++) {
		__asm__ volatile("vrecpe.f32 %P4, %P0\n\tvrecpe.f32 %P6, %P1\n\tvrecpe.f32 %P8, "
		                 "%P2\n\tvrecpe.f32 %P10, %P3\n\t"
		                 "vrecps.f32 %P5, %P0, %P4\n\tvrecps.f32 %P7, %P1, %P6\n\tvrecps.f32 %P9, "
		                 "%P2, %P8\n\tvrecps.f32 %P11, %P3, %P10\n\t"
		                 "vmul.f32 %P4, %P4, %P5\n\tvmul.f32 %P6, %P6, %P7\n\tvmul.f32 %P8, %P8, "
		                 "%P9\n\tvmul.f32 %P10, %P10, %P11\n\t"
		                 "vrecps.f32 %P5, %P0, %P4\n\tvrecps.f32 %P7, %P1, %P6\n\tvrecps.f32 %P9, "
		                 "%P2, %P8\n\tvrecps.f32 %P11, %P3, %P10\n\t"
		                 "vmul.f32 %P4, %P4, %P5\n\tvmul.f32 %P6, %P6, %P7\n\tvmul.f32 %P8, %P8, "
		                 "%P9\n\tvmul.f32 %P10, %P10, %P11\n\t"
		                 "vmul.f32 %P0, %P12, %P4\n\tvmul.f32 %P1, %P12, %P6\n\tvmul.f32 %P2, "
		                 "%P12, %P8\n\tvmul.f32 %P3, %P12, %P10\n\t"
		                 : "+w"(d),
		                   "+w"(d1),
		                   "+w"(d2),
		                   "+w"(d3),
		                   "=&w"(dr),
		                   "=&w"(dt),
		                   "=&w"(er),
		                   "=&w"(et),
		                   "=&w"(fr),
		                   "=&w"(ft),
		                   "=&w"(gr),
		                   "=&w"(gt)
		                 : "w"(da));
	}
	put(r, ISA_RECP_TPUT, cyc() - t0, ITERS * 4u);

	float32x4_t q = vdupq_n_f32(1.5f), q1 = q, qa = vdupq_n_f32(3.0f), qr, qt, sr, st;

	t0 = cyc();
	for (uint32_t i = 0; i < ITERS; i++) {
		__asm__ volatile(R4(RECP("%q0", "%q1", "%q2", "%q3"))
		                 : "+w"(q), "=&w"(qr), "=&w"(qt)
		                 : "w"(qa));
	}
	put(r, ISA_RECP_Q4_LAT, cyc() - t0, ITERS * 16u); /* 4 lanes */
	t0 = cyc();
	for (uint32_t i = 0; i < ITERS; i++) {
		__asm__ volatile(R2("vrecpe.f32 %q2, %q0\n\tvrecpe.f32 %q4, %q1\n\t"
		                    "vrecps.f32 %q3, %q0, %q2\n\tvrecps.f32 %q5, %q1, %q4\n\t"
		                    "vmul.f32 %q2, %q2, %q3\n\tvmul.f32 %q4, %q4, %q5\n\t"
		                    "vrecps.f32 %q3, %q0, %q2\n\tvrecps.f32 %q5, %q1, %q4\n\t"
		                    "vmul.f32 %q2, %q2, %q3\n\tvmul.f32 %q4, %q4, %q5\n\t"
		                    "vmul.f32 %q0, %q6, %q2\n\tvmul.f32 %q1, %q6, %q4\n\t")
		                 : "+w"(q), "+w"(q1), "=&w"(qr), "=&w"(qt), "=&w"(sr), "=&w"(st)
		                 : "w"(qa));
	}
	put(r, ISA_RECP_Q4_TPUT, cyc() - t0, ITERS * 16u); /* 2 chains x 2 steps x 4 lanes */
#undef RECP
	(void)x1, (void)x2, (void)x3;

	/* Accuracy on projection-like operands (screen x = p.x * f / p.z). */
	rs            = 0x2468ACE1u;
	uint32_t mism = 0, maxulp = 0;

	for (uint32_t i = 0; i < 4096u; i++) {
		float       num  = (float)((int32_t)(rnd() % 4000001u) - 2000000) * 0.001f;
		float       den  = 16.0f + (float)(rnd() % 2984000u) * 0.001f;
		float       ieee = num / den;
		float32x2_t b = vdup_n_f32(den), e = vrecpe_f32(b);

		e            = vmul_f32(e, vrecps_f32(b, e));
		e            = vmul_f32(e, vrecps_f32(b, e));
		float    est = num * vget_lane_f32(e, 0);
		uint32_t bi, be;

		memcpy(&bi, &ieee, 4);
		memcpy(&be, &est, 4);
		if (bi != be) {
			uint32_t u = bi > be ? bi - be : be - bi;

			mism++;
			maxulp = u > maxulp ? u : maxulp;
		}
	}
	r->val[ISA_RECP_MISMATCH] = mism;
	r->val[ISA_RECP_MAX_ULP]  = maxulp;
	r->val[ISA_RECP_SAMPLES]  = 4096u;
}

/* ---- VFP scalar multiply on denormals, FPSCR FZ/DN off and on ---- */

static uint32_t get_fpscr(void)
{
	uint32_t v;

	__asm__ volatile("vmrs %0, fpscr" : "=r"(v));
	return v;
}

static void set_fpscr(uint32_t v)
{
	__asm__ volatile("vmsr fpscr, %0" ::"r"(v) : "memory");
}

static uint32_t fmul_chain(float x, uint32_t fpscr, float *out)
{
	float    m    = 1.0f;
	uint32_t save = get_fpscr(), t0;

	set_fpscr(fpscr);
	t0 = cyc();
	for (uint32_t i = 0; i < ITERS; i++) {
		__asm__ volatile(R16("vmul.f32 %0, %0, %1\n\t") : "+t"(x) : "t"(m));
	}
	t0 = cyc() - t0;
	set_fpscr(save);
	*out = x;
	return t0;
}

static void stage_fz(isa_res_t *r)
{
	const uint32_t fzdn   = (1u << 24) | (1u << 25);
	uint32_t       before = get_fpscr(), dbits = 0x000AE398u; /* ~1e-39, subnormal */
	float          den, out;

	memcpy(&den, &dbits, 4);
	put(r, ISA_FMUL_NORM, fmul_chain(1.5f, before & ~fzdn, &out), ITERS * 16u);
	put(r, ISA_FMUL_DENORM, fmul_chain(den, before & ~fzdn, &out), ITERS * 16u);
	put(r, ISA_FMUL_NORM_FZ, fmul_chain(1.5f, before | fzdn, &out), ITERS * 16u);
	put(r, ISA_FMUL_DENORM_FZ, fmul_chain(den, before | fzdn, &out), ITERS * 16u);
	check(r, ISA_FZ_RESTORED, get_fpscr() == before);
}
#else
static void stage_fdiv(isa_res_t *r)
{
	check(r, ISA_FDIV_OK, 1);
}
static void stage_fz(isa_res_t *r)
{
	check(r, ISA_FZ_RESTORED, 1);
}
#endif

/* ---- Gouraud z-tested spans: current (scalar < 8, NEON >= 8 with the
 * masked end since 2026-09-23; the overlapped end before), scalar, and
 * every length in NEON with the last chunk lane-masked. The candidate reads and rewrites up
 * to 7 px past the span end (with the values it read): the band buffers
 * would need 8 px of slack after them. */

#if defined(__ARM_NEON)
static inline __attribute__((always_inline)) void span_gouraud_z_masked(uint16_t *restrict dst,
                                                                        uint16_t *restrict z,
                                                                        int32_t        n,
                                                                        const uint32_t s[PL_N],
                                                                        const plane_t  pl[PL_N])
{
	static const uint16_t idx[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	const uint32_t        gw = pl[PL_W].gx, gr = pl[PL_R].gx, gg = pl[PL_G].gx, gb = pl[PL_B].gx;
	uint32x4_t            w0, w1, r0, r1, g0, g1, b0, b1;
	uint32x4_t            sw = vdupq_n_u32(gw * 8u), sr = vdupq_n_u32(gr * 8u);
	uint32x4_t            sg = vdupq_n_u32(gg * 8u), sb = vdupq_n_u32(gb * 8u);

	lanes(s[PL_W], gw, &w0, &w1);
	lanes(s[PL_R], gr, &r0, &r1);
	lanes(s[PL_G], gg, &g0, &g1);
	lanes(s[PL_B], gb, &b0, &b1);
	for (int32_t i = 0; i < n; i += 8) {
		uint16x8_t w   = hi16(w0, w1);
		uint16x8_t zo  = vld1q_u16(&z[i]);
		uint16x8_t m   = vcgtq_u16(w, zo);
		uint16x8_t col = vorrq_u16(
		    vorrq_u16(vshlq_n_u16(hi16(r0, r1), 11), vshlq_n_u16(hi16(g0, g1), 5)), hi16(b0, b1));

		if (n - i < 8) {
			m = vandq_u16(m, vcltq_u16(vld1q_u16(idx), vdupq_n_u16((uint16_t)(n - i))));
		}
		vst1q_u16(&z[i], vbslq_u16(m, w, zo));
		vst1q_u16(&dst[i], vbslq_u16(m, col, vld1q_u16(&dst[i])));
		w0 = vaddq_u32(w0, sw), w1 = vaddq_u32(w1, sw);
		r0 = vaddq_u32(r0, sr), r1 = vaddq_u32(r1, sr);
		g0 = vaddq_u32(g0, sg), g1 = vaddq_u32(g1, sg);
		b0 = vaddq_u32(b0, sb), b1 = vaddq_u32(b1, sb);
	}
}
#else
#define span_gouraud_z_masked span_gouraud_z_scalar
#endif

#define GZ_REPS 512
#define GZ_BUF  1024
static uint16_t gz_dst[GZ_BUF + 16] __attribute__((aligned(64)));
static uint16_t gz_z[GZ_BUF + 16] __attribute__((aligned(64)));

static void gz_init(void)
{
	rs = 0x13572468u;
	for (int i = 0; i < GZ_BUF + 16; i++) {
		gz_dst[i] = (uint16_t)rnd();
		gz_z[i]   = (uint16_t)(0x4000u + (rnd() & 0x7Fu));
	}
}

static inline __attribute__((always_inline)) uint32_t gz_run(int32_t n, const int variant)
{
	plane_t pl[PL_N] = {
		{ 0, 0x00008000u, 0 }, { 0, 0x00001000u, 0 }, { 0, 0x00000800u, 0 }, { 0, 0x00000C00u, 0 }
	};
	uint32_t t0 = cyc();

	for (uint32_t rep = 0; rep < GZ_REPS; rep++) {
		uint32_t  s[PL_N] = { 0x40000000u + rep * 0x00200000u,
			                  0x00020000u + rep * 0x100u,
			                  0x00100000u,
			                  0x00030000u + rep * 0x80u };
		uint32_t  pos     = (rep * 53u) & 511u;
		uint16_t *d = &gz_dst[pos], *zz = &gz_z[pos];

		if (variant == 0) {
			span_gouraud_z(d, zz, n, s, pl, (int32_t)(GZ_BUF - pos));
		} else if (variant == 1) {
			span_gouraud_z_scalar(d, zz, n, s, pl);
		} else {
			span_gouraud_z_masked(d, zz, n, s, pl);
		}
	}
	return cyc() - t0;
}

static uint32_t gz_crc(void)
{
	return tr_crc32(tr_crc32(0, gz_dst, sizeof(gz_dst)), gz_z, sizeof(gz_z));
}

static void stage_gouraud(isa_res_t *r)
{
	static const int32_t len[6] = { 4, 8, 9, 12, 16, 64 };
	int                  cur_ok = 1, mask_ok = 1;

	for (int k = 0; k < 6; k++) {
		int32_t  n = len[k];
		uint32_t c, ref;

		gz_init();
		c   = gz_run(n, 1);
		ref = gz_crc();
		put(r, ISA_GZ_SCALAR_4 + k, c, GZ_REPS * (uint32_t)n);
		gz_init();
		c = gz_run(n, 0);
		cur_ok &= gz_crc() == ref;
		put(r, ISA_GZ_CUR_4 + k, c, GZ_REPS * (uint32_t)n);
		gz_init();
		c = gz_run(n, 2);
		mask_ok &= gz_crc() == ref;
		put(r, ISA_GZ_MASK_4 + k, c, GZ_REPS * (uint32_t)n);
	}
	r->val[ISA_GZ_SPANS] = GZ_REPS;
	check(r, ISA_GZ_CUR_EXACT, cur_ok);
	check(r, ISA_GZ_MASK_EXACT, mask_ok);
}

/* ---- Per-row edge walk (raster_rows() minus the span), three ways ---- */

#define EDGE_TRIS 256
static tr_tri_setup_t edge_su[EDGE_TRIS];
static tr_tri_t       edge_tri[EDGE_TRIS];

/* Character-sized triangles: 6..40 px wide, 8..56 rows, anywhere on screen. */
static void edge_make(void)
{
	rs = 0xC0FFEE11u;
	for (int i = 0; i < EDGE_TRIS;) {
		tr_tri_t t;
		int32_t  cx = 40 + (int32_t)(rnd() % 640u), cy = 40 + (int32_t)(rnd() % 1180u);
		int32_t  w = 6 + (int32_t)(rnd() % 35u), h = 8 + (int32_t)(rnd() % 49u);

		memset(&t, 0, sizeof(t));
		t.flags = TR_TRI_GOURAUD;
		for (int k = 0; k < 3; k++) {
			t.v[k].x =
			    ((cx + (int32_t)(rnd() % (uint32_t)w) - w / 2) << 4) + (int32_t)(rnd() & 15u);
			t.v[k].y =
			    ((cy + (int32_t)(rnd() % (uint32_t)h) - h / 2) << 4) + (int32_t)(rnd() & 15u);
			t.a[k].w   = (uint16_t)(1000u + (rnd() & 0x3FFFu));
			t.a[k].rgb = (uint16_t)rnd();
		}
		if (edge(t.v[0], t.v[1], t.v[2]) < 0) {
			tr_sv_t v = t.v[1];

			t.v[1] = t.v[2];
			t.v[2] = v;
		}
		if (tri_setup(&edge_su[i], &t, true) && edge_su[i].y0 < edge_su[i].y1) {
			edge_tri[i++] = t;
		}
	}
}

__attribute__((noinline)) static uint32_t edge_cur(const tr_tri_setup_t *su)
{
	edge_dda_t    e[3];
	plane_t       pl[PL_N];
	const int32_t ne = su->ne, nr = su->nr, ax = su->ax;
	uint32_t      acc = 0;

	for (int32_t i = 0; i < ne; i++) {
		e[i] = su->e[i];
	}
	for (int k = 0; k < PL_N; k++) {
		pl[k] = su->pl[k];
	}
	for (int32_t py = su->y0; py < su->y1; py++) {
		int32_t lo = 0, hi_excl = TR_R3D_W, i = 0;

		for (; i < nr; i++) {
			hi_excl = e[i].quot + 1 < hi_excl ? e[i].quot + 1 : hi_excl;
			edge_dda_step(&e[i]);
		}
		for (; i < ne; i++) {
			lo = e[i].quot > lo ? e[i].quot : lo;
			edge_dda_step(&e[i]);
		}
		if (lo < hi_excl) {
			uint32_t sv[PL_N];

			for (int k = 0; k < PL_N; k++) {
				sv[k] = pl[k].row + pl[k].gx * (uint32_t)(lo - ax);
			}
			acc = acc * 31u + (uint32_t)lo + ((uint32_t)hi_excl << 11) +
			      (sv[0] ^ sv[1] ^ sv[2] ^ sv[3]);
		}
		for (int k = 0; k < PL_N; k++) {
			pl[k].row += pl[k].gy;
		}
	}
	return acc;
}

/* Same bits, edge count a compile-time constant: the DDAs and planes live in
 * registers, the per-edge loops unroll. */
static inline __attribute__((always_inline)) uint32_t edge_reg_n(const tr_tri_setup_t *su,
                                                                 const int             nr,
                                                                 const int             ne)
{
	int32_t  q[3], rm[3], sq[3], sr[3], dn[3];
	uint32_t row[PL_N], gx[PL_N], gy[PL_N], acc = 0;
	int32_t  ax = su->ax;

	for (int i = 0; i < ne; i++) {
		q[i] = su->e[i].quot, rm[i] = su->e[i].rem, sq[i] = su->e[i].step_quot,
		sr[i] = su->e[i].step_rem, dn[i] = su->e[i].denom;
	}
	for (int k = 0; k < PL_N; k++) {
		row[k] = su->pl[k].row, gx[k] = su->pl[k].gx, gy[k] = su->pl[k].gy;
	}
	for (int32_t py = su->y0; py < su->y1; py++) {
		int32_t lo = 0, hi_excl = TR_R3D_W;

		for (int i = 0; i < ne; i++) {
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
			uint32_t d = (uint32_t)(lo - ax);

			acc = acc * 31u + (uint32_t)lo + ((uint32_t)hi_excl << 11) +
			      ((row[0] + gx[0] * d) ^ (row[1] + gx[1] * d) ^ (row[2] + gx[2] * d) ^
			       (row[3] + gx[3] * d));
		}
		for (int k = 0; k < PL_N; k++) {
			row[k] += gy[k];
		}
	}
	return acc;
}

__attribute__((noinline)) static uint32_t edge_reg(const tr_tri_setup_t *su)
{
	if (su->ne == 2) {
		return edge_reg_n(su, 1, 2);
	}
	return su->nr == 1 ? edge_reg_n(su, 1, 3) : edge_reg_n(su, 2, 3);
}

/* 16.16 fixed-point bounds (x += slope): NOT exact -- counts rows whose
 * [lo, hi) differs from the exact DDA's. */
static inline void fix_init(const edge_dda_t *e, int32_t *x16, int32_t *d16)
{
	*x16 = e->quot * 65536 + (int32_t)(((int64_t)e->rem << 16) / e->denom);
	*d16 = e->step_quot * 65536 + (int32_t)(((int64_t)e->step_rem << 16) / e->denom);
}

__attribute__((noinline)) static uint32_t edge_fix(const tr_tri_setup_t *su, uint32_t *mism)
{
	int32_t       x[3], d[3];
	edge_dda_t    e[3];
	const int32_t ne = su->ne, nr = su->nr;
	uint32_t      acc = 0;

	for (int32_t i = 0; i < ne; i++) {
		fix_init(&su->e[i], &x[i], &d[i]);
		e[i] = su->e[i];
	}
	for (int32_t py = su->y0; py < su->y1; py++) {
		int32_t lo = 0, hi_excl = TR_R3D_W, i = 0;

		for (; i < nr; i++) {
			hi_excl = (x[i] >> 16) + 1 < hi_excl ? (x[i] >> 16) + 1 : hi_excl;
			x[i] += d[i];
		}
		for (; i < ne; i++) {
			lo = (x[i] >> 16) > lo ? (x[i] >> 16) : lo;
			x[i] += d[i];
		}
		acc = acc * 31u + (uint32_t)lo + ((uint32_t)hi_excl << 11);
		if (mism != NULL) { /* untimed pass: compare with the exact DDA */
			int32_t elo = 0, ehi = TR_R3D_W, j = 0;

			for (; j < nr; j++) {
				ehi = e[j].quot + 1 < ehi ? e[j].quot + 1 : ehi;
				edge_dda_step(&e[j]);
			}
			for (; j < ne; j++) {
				elo = e[j].quot > elo ? e[j].quot : elo;
				edge_dda_step(&e[j]);
			}
			if ((lo < hi_excl || elo < ehi) && (lo != elo || hi_excl != ehi)) {
				(*mism)++;
			}
		}
	}
	return acc;
}

static void stage_edge(isa_res_t *r)
{
	enum { PASSES = 8 };
	uint32_t rows = 0, a0 = 0, a1 = 0, mism = 0, t0;

	edge_make();
	for (int i = 0; i < EDGE_TRIS; i++) {
		rows += (uint32_t)(edge_su[i].y1 - edge_su[i].y0);
		(void)edge_fix(&edge_su[i], &mism);
	}
	t0 = cyc();
	for (int p = 0; p < PASSES; p++) {
		for (int i = 0; i < EDGE_TRIS; i++) {
			a0 += edge_cur(&edge_su[i]);
		}
	}
	put(r, ISA_EDGE_CUR, cyc() - t0, rows * PASSES);
	t0 = cyc();
	for (int p = 0; p < PASSES; p++) {
		for (int i = 0; i < EDGE_TRIS; i++) {
			a1 += edge_reg(&edge_su[i]);
		}
	}
	put(r, ISA_EDGE_REG, cyc() - t0, rows * PASSES);
	t0 = cyc();
	for (int p = 0; p < PASSES; p++) {
		for (int i = 0; i < EDGE_TRIS; i++) {
			(void)edge_fix(&edge_su[i], NULL);
		}
	}
	put(r, ISA_EDGE_FIX, cyc() - t0, rows * PASSES);
	t0 = cyc();
	for (int p = 0; p < PASSES; p++) {
		for (int i = 0; i < EDGE_TRIS; i++) {
			(void)tri_setup(&edge_su[i], &edge_tri[i], true);
		}
	}
	put(r, ISA_TRI_SETUP, cyc() - t0, EDGE_TRIS * PASSES);
	r->val[ISA_EDGE_ROWS]         = rows;
	r->val[ISA_EDGE_FIX_MISMATCH] = mism;
	check(r, ISA_EDGE_REG_EXACT, a0 == a1);
}

/* ---- NOZ texture gather (tex_run_noz), with and without PLD ---- */

#define TEX_ROWS 64
static uint16_t tex0[TR_TEX_DIM * TR_TEX_DIM] __attribute__((aligned(64)));
static uint16_t tex_out[TEX_ROWS * TR_R3D_W] __attribute__((aligned(64)));

static inline __attribute__((always_inline)) void tex_rows(const int pld)
{
	const uint8_t *tb = (const uint8_t *)tex0;

	for (uint32_t y = 0; y < TEX_ROWS; y++) {
		uint32_t ua = y * 9001u, va = y * 40000u, du = 21300u, dv = 1638u + y * 16u;

		for (int32_t x = 0; x < TR_R3D_W; x += 8) {
			if (pld) { /* two sub-spans ahead */
				uint32_t u2 = ua + 16u * du, v2 = va + 16u * dv;

				__builtin_prefetch(tb + (((v2 >> 6) & ((TR_TEX_DIM - 1) << 8)) |
				                         ((u2 >> 13) & ((TR_TEX_DIM - 1) << 1))));
			}
			tex_run_noz(&tex_out[y * TR_R3D_W + (uint32_t)x], 8, ua, du, va, dv, tex0);
			ua += 8u * du;
			va += 8u * dv;
		}
	}
}

#if ISA_PAYLOAD
/* Evict L1 + L2: read 1 MiB of SRAM1 0x02600000.. (WB-WA XN in the stub
 * table; FB B, read only -- no line is dirtied), one load per 64 B line. Not
 * MRAM: its low sectors hold TF-A, which may be walled off from NS. */
static void evict(void)
{
	const volatile uint32_t *m   = (const volatile uint32_t *)0x02600000u;
	uint32_t                 acc = 0;

	for (uint32_t i = 0; i < (1u << 20) / 4u; i += 16u) {
		acc += m[i];
	}
	__asm__ volatile("" ::"r"(acc));
}
#else
static void evict(void)
{
}
#endif

static void stage_tex(isa_res_t *r)
{
	uint32_t t0, ref;

	tr_golden_tex_init(tex0);
	tex_rows(0);
	t0 = cyc();
	tex_rows(0);
	put(r, ISA_TEX_WARM, cyc() - t0, TEX_ROWS * TR_R3D_W);
	ref = tr_crc32(0, tex_out, sizeof(tex_out));
	memset(tex_out, 0, sizeof(tex_out));
	t0 = cyc();
	tex_rows(1);
	put(r, ISA_TEX_WARM_PLD, cyc() - t0, TEX_ROWS * TR_R3D_W);
	check(r, ISA_TEX_PLD_EXACT, tr_crc32(0, tex_out, sizeof(tex_out)) == ref);
	evict();
	t0 = cyc();
	tex_rows(0);
	put(r, ISA_TEX_COLD, cyc() - t0, TEX_ROWS * TR_R3D_W);
	evict();
	t0 = cyc();
	tex_rows(1);
	put(r, ISA_TEX_COLD_PLD, cyc() - t0, TEX_ROWS * TR_R3D_W);
}

/* ---- Integer divides in tri_setup(): 32-bit UDIV, the per-triangle
 * R = 2^62 / area2 (uint64 `/`, a libgcc call on the A32), recip62() (the
 * renderer's since 2026-09-23), and tri_setup() itself on the edge stage's triangles ---- */

/* recip62(): r3d_raster.c's own (included above). */
#define DIV_N 1024
static uint64_t div_a[DIV_N], div_q[DIV_N];

static void stage_div(isa_res_t *r)
{
	uint32_t t0, ok = 1, x = 46341u, k = 0x7FFFFFFFu;

#if defined(__arm__)
	t0 = cyc();
	for (uint32_t i = 0; i < 64u; i++) {
		__asm__ volatile(R16("udiv %0, %1, %0\n\t") : "+r"(x) : "r"(k));
	}
	put(r, ISA_UDIV_LAT, cyc() - t0, 64u * 16u);
#endif
	(void)k;
	rs = 0x5EED1234u;
	for (int i = 0; i < DIV_N; i++) { /* area2 in 28.4^2: a 2 px^2 sliver .. a full-screen quad */
		uint32_t sh = rnd() % 26u;

		div_a[i] = 1024u + ((uint64_t)rnd() >> (31u - sh)) * 7u;
	}
	t0 = cyc();
	for (int i = 0; i < DIV_N; i++) {
		div_q[i] = ((uint64_t)1 << 62) / div_a[i];
	}
	put(r, ISA_LDIV64, cyc() - t0, DIV_N);
	t0 = cyc();
	for (int i = 0; i < DIV_N; i++) {
		x ^= (uint32_t)recip62(div_a[i]);
	}
	put(r, ISA_RECIP62_FAST, cyc() - t0, DIV_N);
	for (int i = 0; i < DIV_N; i++) {
		ok &= recip62(div_a[i]) == div_q[i];
	}
	for (uint64_t a = 1; a < 5000u; a++) {
		ok &= recip62(a) == ((uint64_t)1 << 62) / a;
	}
	check(r, ISA_RECIP62_EXACT, ok);
	r->val[ISA_GZ_SPANS] += x & 0u;
}

/* ---- Constant-w fogged ground row: generic sub-span loop vs span_tex() ---- */

#define GS_ROWS 16
static uint8_t  gs_idx[TR_TEX_DIM * TR_TEX_DIM];
static uint16_t gs_pal[TR_FOG_LEVELS * TR_FOG_PAL_MAX];
static uint16_t gs_out[2][GS_ROWS * TR_R3D_W] __attribute__((aligned(64)));

static inline __attribute__((always_inline)) void
gspan_rows(uint16_t *out, const tr_tex_fog_t *fog, const int cw)
{
	const uint32_t W  = 0x01F40000u; /* w 500: fog level from tr_r3d_fog_lut[31] */
	persp_t        pw = persp_w(W);
	plane_t        pl[PL_N];

	pl[PL_W].gx = 0;
	pl[PL_U].gx = (uint32_t)(((int64_t)2600 << pw.rs) / pw.rw); /* ~1.3 texel/px */
	pl[PL_V].gx = (uint32_t)(((int64_t)200 << pw.rs) / pw.rw);
	for (uint32_t y = 0; y < GS_ROWS; y++) {
		uint32_t s[PL_N] = { W, y * 12345u, y * 54321u, 0 };

		if (cw) {
			span_tex(&out[y * TR_R3D_W], TR_R3D_W, s, pl, NULL, fog);
		} else {
			span_tex_z_impl(&out[y * TR_R3D_W], NULL, TR_R3D_W, s, pl, NULL, 0, 1, fog);
		}
	}
}

static void stage_gspan(isa_res_t *r)
{
	tr_tex_fog_t fog = { gs_idx, gs_pal, TR_FOG_PAL_MAX };
	uint32_t     t0;

	for (uint32_t i = 0; i < TR_TEX_DIM * TR_TEX_DIM; i++) {
		gs_idx[i] = (uint8_t)(((i * 2654435761u) >> 27) * 2u); /* palette entry * 2 */
	}
	for (uint32_t i = 0; i < TR_FOG_LEVELS * TR_FOG_PAL_MAX; i++) {
		gs_pal[i] = (uint16_t)(i * 0x9E37u);
	}
	for (uint32_t i = 0; i < TR_FOG_LUT_N; i++) {
		tr_r3d_fog_lut[i] = 3;
	}
	gspan_rows(gs_out[0], &fog, 0); /* warm */
	t0 = cyc();
	gspan_rows(gs_out[0], &fog, 0);
	put(r, ISA_GSPAN_OLD, cyc() - t0, GS_ROWS * TR_R3D_W);
	t0 = cyc();
	gspan_rows(gs_out[1], &fog, 1);
	put(r, ISA_GSPAN_CW, cyc() - t0, GS_ROWS * TR_R3D_W);
	check(r, ISA_GSPAN_EXACT, memcmp(gs_out[0], gs_out[1], sizeof(gs_out[0])) == 0);
}

/* ---- Band-sized fills: the z clear / background store forms ---- */

static uint16_t fill_buf[BAND_PX] __attribute__((aligned(64)));

static inline __attribute__((always_inline)) void fill_one(uint8_t *p, uint32_t n, const int how)
{
#if defined(__arm__) && defined(__ARM_NEON)
	if (how == 1) {
		uint16x8_t v = vdupq_n_u16(0);

		for (uint32_t x = 0; x < n / 2u; x += 8) {
			vst1q_u16((uint16_t *)(void *)p + x, v);
		}
	} else if (how == 2) {
		__asm__ volatile("vmov.i8 q0, #0\n\tvmov.i8 q1, #0\n"
		                 "1:\tvst1.64 {d0-d3}, [%0:256]!\n\tsubs %1, %1, #32\n\tbgt 1b"
		                 : "+r"(p), "+r"(n)::"d0", "d1", "d2", "d3", "cc", "memory");
	} else if (how == 3) {
		__asm__ volatile(
		    "vmov.i8 q0, #0\n\tvmov.i8 q1, #0\n\tvmov.i8 q2, #0\n\tvmov.i8 q3, #0\n"
		    "1:\tvstmia %0!, {d0-d7}\n\tsubs %1, %1, #64\n\tbgt 1b"
		    : "+r"(p), "+r"(n)::"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "cc", "memory");
	} else if (how == 4) {
		__asm__ volatile(
		    "mov r2, #0\n\tmov r3, #0\n"
		    "1:\tstrd r2, r3, [%0], #8\n\tstrd r2, r3, [%0], #8\n\t"
		    "strd r2, r3, [%0], #8\n\tstrd r2, r3, [%0], #8\n\tsubs %1, %1, #32\n\tbgt 1b"
		    : "+r"(p), "+r"(n)::"r2", "r3", "cc", "memory");
	} else
#endif
	{
		(void)how;
		memset(p, 0, n);
	}
}

static int fill_zero(uint32_t n)
{
	const uint32_t *w = (const uint32_t *)(const void *)fill_buf;

	for (uint32_t i = 0; i < n / 4u; i++) {
		if (w[i] != 0) {
			return 0;
		}
	}
	return 1;
}

static inline __attribute__((always_inline)) uint32_t fill_time(uint32_t  n,
                                                                uint32_t  reps,
                                                                const int how,
                                                                int      *ok)
{
	uint32_t t0;

	memset(fill_buf, 0xFF, sizeof(fill_buf));
	fill_one((uint8_t *)fill_buf, n, how); /* warm */
	memset(fill_buf, 0xFF, sizeof(fill_buf));
	t0 = cyc();
	for (uint32_t i = 0; i < reps; i++) {
		fill_one((uint8_t *)fill_buf, n, how);
	}
	t0 = cyc() - t0;
	*ok &= fill_zero(n);
	return t0;
}

static void stage_fill(isa_res_t *r)
{
	int ok = 1;

	_Static_assert(BAND_PX * 2 % 64 == 0 && 16384 % 64 == 0, "fill sizes are whole 64 B blocks");
	put(r, ISA_FILL16K_MEMSET, fill_time(16384u, 64u, 0, &ok), 16384u * 64u);
	put(r, ISA_FILL16K_VST1Q, fill_time(16384u, 64u, 1, &ok), 16384u * 64u);
	put(r, ISA_FILL16K_VST256, fill_time(16384u, 64u, 2, &ok), 16384u * 64u);
	put(r, ISA_FILL16K_VSTM, fill_time(16384u, 64u, 3, &ok), 16384u * 64u);
	put(r, ISA_FILL16K_STRD, fill_time(16384u, 64u, 4, &ok), 16384u * 64u);
	put(r, ISA_FILL45K_MEMSET, fill_time(BAND_PX * 2u, 32u, 0, &ok), BAND_PX * 2u * 32u);
	put(r, ISA_FILL45K_VST1Q, fill_time(BAND_PX * 2u, 32u, 1, &ok), BAND_PX * 2u * 32u);
	put(r, ISA_FILL45K_VST256, fill_time(BAND_PX * 2u, 32u, 2, &ok), BAND_PX * 2u * 32u);
	put(r, ISA_FILL45K_VSTM, fill_time(BAND_PX * 2u, 32u, 3, &ok), BAND_PX * 2u * 32u);
	put(r, ISA_FILL45K_STRD, fill_time(BAND_PX * 2u, 32u, 4, &ok), BAND_PX * 2u * 32u);
	check(r, ISA_FILL_OK, ok);
}

/* ---- The golden frame: setup + bin, 40 bands, per-band CRC ---- */

static tr_dl_t        gdl;
static tr_tri_setup_t gsetup[TR_GOLDEN_DL_N];
static uint16_t       gbins[TR_BANDS][TR_BIN_MAX];
static uint32_t       gcounts[TR_BANDS];
static uint16_t       gband[2 * BAND_PX] __attribute__((aligned(64)));

static void stage_golden(isa_res_t *r)
{
	static const tr_bg_t  bg         = TR_GOLDEN_BG;
	static const uint32_t band_crc[] = TR_GOLDEN_BAND_CRC;
	uint32_t              best_s = 0xFFFFFFFFu, best_r = 0xFFFFFFFFu, ok = 0;

	memcpy(gdl.tri, tr_golden_dl, sizeof(tr_golden_dl));
	gdl.n = TR_GOLDEN_DL_N;
	tr_golden_tex_init(tex0);
	tr_r3d_tex[0] = tex0;
	for (int pass = 0; pass < 5; pass++) {
		uint32_t ovf = 0, t0 = cyc(), tr = 0;

		tr_bin_build(&gdl, gsetup, gbins, gcounts, &ovf);
		t0     = cyc() - t0;
		best_s = t0 < best_s ? t0 : best_s;
		ok     = 0;
		for (int b = 0; b < TR_BANDS; b++) {
			uint32_t t1 = cyc();

			tr_raster_band(NULL,
			               0,
			               b * TR_BAND_H,
			               (b + 1) * TR_BAND_H,
			               gband,
			               gband + BAND_PX,
			               &bg,
			               &gdl,
			               gsetup,
			               gbins[b],
			               gcounts[b]);
			tr += cyc() - t1;
			ok += tr_crc32(0, gband + BAND_PX, BAND_PX * 2u) == band_crc[b];
		}
		best_r = tr < best_r ? tr : best_r;
	}
	put(r, ISA_GOLD_SETUP, best_s, TR_GOLDEN_DL_N);
	put(r, ISA_GOLD_RASTER, best_r, TR_R3D_W * TR_R3D_H);
	r->val[ISA_GOLD_TRIS]     = TR_GOLDEN_DL_N;
	r->val[ISA_GOLD_BANDS_OK] = ok;
	check(r, ISA_GOLDEN_OK, ok == TR_BANDS);
}

void isa_bench_run(isa_res_t *r)
{
	static void (*const stage[])(isa_res_t *) = { stage_pmu,  stage_fdiv,  stage_fz,  stage_gouraud,
		                                          stage_edge, stage_div,   stage_tex, stage_gspan,
		                                          stage_fill, stage_golden };

	r->opt = ISA_OPT;
	for (uint32_t s = 0; s < sizeof(stage) / sizeof(stage[0]); s++) {
		r->stage = s + 1u;
		stage[s](r);
	}
	r->stage = ISA_STAGE_END;
}

#ifdef ISA_HOST_MAIN
#include <stdio.h>

/* `make check`: host cc and qemu-arm (rdimon). Every check must pass. */
int main(void)
{
	static isa_res_t         r;
	static const char *const names[] = {
#define ISA_NAME(n) #n,
		ISA_CHECKS(ISA_NAME)
#undef ISA_NAME
	};

	isa_bench_run(&r);
	for (int i = 0; i < ISA_C_N; i++) {
		printf("%-16s %s\n",
		       names[i],
		       (r.fail >> i) & 1u   ? "FAIL"
		       : (r.pass >> i) & 1u ? "pass"
		                            : "not run");
	}
	printf("recp mismatch %u/%u (max %u ulp), fixed-point edge mismatch %u/%u rows, golden bands "
	       "%u/40\n",
	       (unsigned)r.val[ISA_RECP_MISMATCH],
	       (unsigned)r.val[ISA_RECP_SAMPLES],
	       (unsigned)r.val[ISA_RECP_MAX_ULP],
	       (unsigned)r.val[ISA_EDGE_FIX_MISMATCH],
	       (unsigned)r.val[ISA_EDGE_ROWS],
	       (unsigned)r.val[ISA_GOLD_BANDS_OK]);
	return (r.fail == 0 && r.pass == (1u << ISA_C_N) - 1u && r.stage == ISA_STAGE_END) ? 0 : 1;
}
#endif
