/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Word-copy memcpy core (issue #2285).
 *
 * WHY THIS EXISTS: on an Alif Ensemble (AEN) -Os Zephyr image,
 * CONFIG_SIZE_OPTIMIZATIONS links picolibc's "space" multilib, whose
 * memcpy() copies one byte at a time. Bench run 223 (camera-mjpeg-stream)
 * measured a 24-30 ms HTTP send per 32-41 KB frame with 45-51% of frames
 * dropped; the buffer-starvation fix in this same change (Ethernet TX/RX
 * buffer sizing) alone took those drops to 0% -- that is the only
 * bench-proven fix in this change. A same-scene A/B since run (E1M-AEN803,
 * module 2026W36-0001, 640x480 @ 30 fps, camera-bound) against that
 * buffer-sizing-only baseline found no measurable gain from this
 * word-copy override: A1 (memcpy ON) 30.03 fps / 1,089,750 B/s, B1
 * (memcpy OFF) 30.03 fps / 1,092,219 B/s, A2 (memcpy ON) 30.03 fps /
 * 1,088,302 B/s, 0 drops throughout. The send-bound case -- the one run
 * 223 actually hit -- is still unmeasured. The 60.1 fps figure (bench
 * runs 224-228, see the Kconfig help) belongs to the combined
 * buffer-sizing-plus-word-copy configuration on a smaller-frame scene,
 * not to this word-copy override on its own. The full
 * CONFIG_SPEED_OPTIMIZATIONS build copies faster but overflows this
 * app's ITCM budget by about 41 KB even when RAM-run, so it is not an
 * option here. This word-at-a-time loop is meant as the middle ground --
 * better throughput than the byte loop at -Os code size, without that
 * overflow -- but that is intent, not a result the A/B above bore out.
 *
 * THE SELF-RECURSION TRAP: GCC's -ftree-loop-distribute-patterns pass can
 * recognise a byte-copying loop shape and rewrite it into a call to
 * memcpy(). That is fine in ordinary code, but this file's whole purpose
 * is a loop body that runs as a loop -- if the loop below got rewritten,
 * it would call memcpy() again, which calls into this same loop again,
 * forever. The alp_fast_memcpy_core() function below carries
 * "no-tree-loop-distribute-patterns" as insurance against exactly that.
 * Do NOT read this as an -Os hazard: confirmed against GCC 14.3
 * (tests/scripts/test_fast_memcpy_core_build_guard.py), this pass does
 * not rewrite this loop shape at the TU's own plain -Os -- reproducing
 * the rewrite at all needs a -O2 compile without -ffreestanding. What
 * the attribute actually guards is a future -O2 build of this TU and any
 * future edit to the loop shape below that a different GCC version's
 * -Os pass decides to rewrite. This file originally paired the guard
 * with a local optimize("O2") (see below) to keep decent codegen despite
 * the surrounding -Os image, and it was under that O2 pairing that the
 * recursion was seen for real on the bench (run 227): the board hung
 * before the network came up, hard-faulting silently on the recursive
 * blowing of the stack.
 *
 * GCC ONLY: this file no longer pairs the guard with a local
 * optimize("O2") -- an earlier version did, and that "O2" (not -Os
 * itself, and not the recursion guard above) is what let GCC
 * auto-vectorize the loop to Arm MVE Cortex-M55 Q-register
 * vldrw/vstrw, putting memcpy() on FPU/MVE state everywhere it runs:
 * an ISR built with FPU_SHARING=n could clobber it, and a call before
 * the CPACR coprocessor-access enable (e.g. on the SRAM_VECTOR_TABLE
 * relocate path) NOCP UsageFaults. Dropped; this function now compiles
 * at the surrounding TU's own -Os, plain word ldr/str, no MVE. Clang's
 * -Os loop-idiom pass would still rewrite this loop into a memcpy()
 * call regardless of the GCC-specific guard above, recursing the same
 * way run 227 did -- so this file refuses to build under Clang outright
 * (belt-and-suspenders alongside CONFIG_ALP_SDK_FAST_MEMCPY's Kconfig
 * `depends on` on the GCC toolchain variant).
 *
 * NOT a memmove(): like the C standard memcpy(), overlapping @p dst /
 * @p src is undefined behaviour here -- this function does not detect
 * or handle overlap, and must never be aliased onto memmove().
 */

#include "alp_fast_memcpy_core.h"

#include <stdint.h>

#if defined(__clang__)
#error "alp_fast_memcpy_core.c is GCC-only -- see the self-recursion trap in this file's header"
#endif

/* Test-only alignment guard, compiled in only by the native_sim host
 * unit test (tests/unit/fast_memcpy/CMakeLists.txt defines
 * ALP_FAST_MEMCPY_CORE_ASSERT_ALIGN on that target alone -- never on a
 * real Ensemble image). A mutated prologue below (e.g. masking with 2
 * instead of 3, or checking @p s instead of @p d) can still copy every
 * BYTE to the right place on a host that tolerates unaligned word
 * accesses, so the exhaustive value comparison in the test wouldn't
 * catch it; this aborts the moment the word loop would run against an
 * unaligned pointer, independent of whether the resulting bytes happen
 * to come out right. */
#ifdef ALP_FAST_MEMCPY_CORE_ASSERT_ALIGN
#include <stdlib.h>
#define ALP_FAST_MEMCPY_ASSERT_WORD_ALIGNED(p) \
	do { \
		if ((((uintptr_t)(p)) & 3U) != 0U) { \
			abort(); \
		} \
	} while (0)
#else
#define ALP_FAST_MEMCPY_ASSERT_WORD_ALIGNED(p) ((void)0)
#endif

__attribute__((optimize("no-tree-loop-distribute-patterns"))) void *
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

		/* Only assert here when the word loops below are actually
		 * about to dereference dw/sw (n >= 4) -- the prologue above
		 * can also exit with n exhausted (0-3 bytes left) before d
		 * ever reaches word alignment, in which case dw/sw are
		 * computed but never read, and asserting unconditionally
		 * would fire on that legitimate short-buffer case. */
		if (n >= 4U) {
			ALP_FAST_MEMCPY_ASSERT_WORD_ALIGNED(dw);
			ALP_FAST_MEMCPY_ASSERT_WORD_ALIGNED(sw);
		}

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
