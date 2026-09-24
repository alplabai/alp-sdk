/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Word-copy memcpy core (issue #2285).
 *
 * WHY THIS EXISTS: on an Alif Ensemble (AEN) -Os Zephyr image,
 * CONFIG_SIZE_OPTIMIZATIONS links picolibc's "space" multilib, whose
 * memcpy() copies one byte at a time -- the dominant cost in the
 * camera-mjpeg-stream JPEG-encode/HTTP-send loop (bench run 223:
 * 24-30 ms send time per 32-41 KB frame, 45-51% of frames dropped
 * against a 60 fps camera). The full CONFIG_SPEED_OPTIMIZATIONS build
 * copies faster but overflows this app's ITCM by about 41 KB, so it is
 * not an option here. This word-at-a-time loop is the middle ground:
 * near-speed-build throughput at -Os code size.
 *
 * THE SELF-RECURSION TRAP: GCC's -ftree-loop-distribute-patterns pass
 * (on by default from -O2, and pulled in by -Os with -ftree-vectorize)
 * recognises a byte-copying loop shape and rewrites it into a call to
 * memcpy(). That is fine in ordinary code, but this file's whole
 * purpose is faster loop bodies with no help from the same pass -- if
 * the loop below got rewritten, it would call memcpy() again, which
 * calls into this same loop again, forever. The alp_fast_memcpy_core()
 * function below carries "no-tree-loop-distribute-patterns" for exactly
 * this reason. Seen for real on the bench (run 227): the board hung
 * before the network came up, hard-faulting silently on the recursive
 * blowing of the stack. "O2" is paired with it (rather than leaving the
 * surrounding TU's own -Os) so the word/loop body itself still gets
 * decent codegen despite the whole image building at -Os.
 *
 * NOT a memmove(): like the C standard memcpy(), overlapping @p dst /
 * @p src is undefined behaviour here -- this function does not detect
 * or handle overlap, and must never be aliased onto memmove().
 */

#include "alp_fast_memcpy_core.h"

#include <stdint.h>

__attribute__((optimize("O2", "no-tree-loop-distribute-patterns"))) void *
alp_fast_memcpy_core(void *restrict dst, const void *restrict src, size_t n)
{
	uint8_t       *d = dst;
	const uint8_t *s = src;

	/* Word path only when src and dst share the same alignment phase
	 * (mod 4) -- a fixed misalignment between them can still be walked
	 * a word at a time once the shared low bits are worked off below;
	 * an UNSHARED phase (e.g. dst word-aligned, src off by 1) cannot,
	 * so those pairs fall straight through to the byte loop at the
	 * bottom. That byte loop is the one case that keeps this file's
	 * whole reason to exist (the picolibc "space" byte loop) -- but
	 * only for the leading/trailing remainder, not the whole buffer. */
	if ((((uintptr_t)d ^ (uintptr_t)s) & 3U) == 0U) {
		while (n && ((uintptr_t)d & 3U)) {
			*d++ = *s++;
			n--;
		}

		uint32_t       *dw = (uint32_t *)d;
		const uint32_t *sw = (const uint32_t *)s;

		/* Four words per iteration -- cuts loop-branch overhead
		 * fourfold over a plain word-at-a-time loop for the frame
		 * sizes this app copies (tens of KB). */
		while (n >= 16U) {
			dw[0] = sw[0];
			dw[1] = sw[1];
			dw[2] = sw[2];
			dw[3] = sw[3];
			dw += 4;
			sw += 4;
			n -= 16U;
		}
		while (n >= 4U) {
			*dw++ = *sw++;
			n -= 4U;
		}

		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}

	while (n--) {
		*d++ = *s++;
	}

	return dst;
}
