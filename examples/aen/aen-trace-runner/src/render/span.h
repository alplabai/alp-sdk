/* src/render/span.h -- tr_span_fill(): write `n` copies of RGB565 colour `c`
 * starting at `dst`. The one Helium file in the renderer (see the real-3D
 * plan, section 5, T3): every constant-colour horizontal span the
 * rasterizer draws, plus the sky gradient's per-row fill, goes through this
 * one function, so there is exactly one place that needs the MVE intrinsics
 * and exactly one place a fallback has to match.
 *
 * `n` may be 0 (writes nothing) and `dst` need not be aligned -- the tail
 * predicate handles both the not-a-multiple-of-8 remainder AND an n < 8
 * span in the same code path, so callers never special-case a short span.
 *
 * Three branches: Helium (M55), NEON (A32: 8-px stores + one overlapping
 * tail store), scalar (host). tr_span_selfcheck() below proves whichever
 * one is compiled against a plain loop.
 *
 * Deliberately free of Zephyr/alp-sdk headers, same as proj.c, so
 * tests/host/runner.sh can compile this straight into a host test; on host
 * (__ARM_FEATURE_MVE undefined) the #else scalar loop is what runs and is
 * exactly what test_r3d_raster.c's `tr_span_fill == reference` case checks
 * against a hand-written reference loop.
 */
#ifndef TR_SPAN_H
#define TR_SPAN_H

#include <stdint.h>

#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE & 1)

#include <arm_mve.h> /* needs CONFIG_FPU=y, prj.conf (see probe/fillrate/prj.conf) */

static inline void tr_span_fill(uint16_t *dst, uint32_t n, uint16_t c)
{
	uint16x8_t v = vdupq_n_u16(c);
	uint32_t   i = 0;

	/* One 128-bit VSTRH per 8 px -- the widest halfword store the M55
	 * issues, same reasoning as probe/fillrate's timed_fill_mve(). Zephyr's
	 * stdint.h typedefs collide with arm_mve.h's own pointer types unless
	 * every MVE intrinsic pointer argument is cast through (void *) --
	 * see probe/fillrate/src/main.c's timed_fill_mve() for the same cast. */
	for (; i + 8 <= n; i += 8) {
		vst1q_u16((void *)&dst[i], v);
	}

	/*
	 * Tail: 0..7 leftover px, predicated in ONE more 128-bit store instead
	 * of a scalar tail loop -- vctp16q(n - i) sets exactly the low
	 * (n - i) predicate lanes true (n - i is 0..7 here, never >= 8, so this
	 * never predicates in an eighth lane that belongs to the next span).
	 * A span of n < 8 falls straight into this with i == 0 and no
	 * fixed-width loop above ever running -- there is no separate "short
	 * span" path to fall out of sync with the long one.
	 */
	if (i < n) {
		mve_pred16_t p = vctp16q(n - i);

		vstrhq_p_u16((void *)&dst[i], v, p);
	}
}

#elif defined(__ARM_NEON)

#include <arm_neon.h> /* Cortex-A32 renderer: -mfpu=neon-fp-armv8 */

static inline void tr_span_fill(uint16_t *dst, uint32_t n, uint16_t c)
{
	if (n < 8) {
		for (uint32_t i = 0; i < n; i++) {
			dst[i] = c;
		}
		return;
	}

	uint16x8_t v = vdupq_n_u16(c);
	uint32_t   i = 0;

	for (; i + 8 <= n; i += 8) {
		vst1q_u16(&dst[i], v);
	}
	/* Tail: no predication on A32, so ONE more 8-px store ending exactly at
	 * dst[n - 1] -- it rewrites up to 7 px already filled with the same
	 * colour, never a pixel past the span (n >= 8 here). */
	if (i < n) {
		vst1q_u16(&dst[n - 8], v);
	}
}

#else

static inline void tr_span_fill(uint16_t *dst, uint32_t n, uint16_t c)
{
	for (uint32_t i = 0; i < n; i++) {
		dst[i] = c;
	}
}

#endif

/*
 * Boot self-check for whichever tr_span_fill() branch this build compiled:
 * every n in 0..40 at every halfword offset 1..8 of a buffer (so every
 * alignment mod 16 bytes), against a plain loop, with guard pixels either side. Returns 1 on pass, 0 on the
 * first mismatch. Host runs it on the scalar branch; the A32 renderer calls
 * it once at entry (NEON branch) and reports a 0 as a fault.
 */
static inline int tr_span_selfcheck(void)
{
	uint16_t buf[64], ref[64];

	for (uint32_t off = 0; off < 8; off++) {
		for (uint32_t n = 0; n <= 40; n++) {
			for (uint32_t i = 0; i < 64; i++) {
				buf[i] = ref[i] = (uint16_t)(0xA000u + i);
			}
			tr_span_fill(&buf[off + 1], n, 0x5A5Au);
			for (uint32_t i = 0; i < n; i++) {
				ref[off + 1 + i] = 0x5A5Au;
			}
			for (uint32_t i = 0; i < 64; i++) {
				if (buf[i] != ref[i]) {
					return 0;
				}
			}
		}
	}
	return 1;
}

#endif /* TR_SPAN_H */
