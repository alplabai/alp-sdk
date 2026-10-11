/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file camera_cr10_bench.c
 * @brief Microbenchmark: CR10 unpack, scalar vs NEON, at the real sensor frame size (#2792).
 *
 * Usage: camera_cr10_bench [width height out_bytes shift]   (default 1456 1088 2 0)
 *
 * Unpacks a CACHED frame (L2/DRAM-backed ordinary memory), so it measures the
 * ALU cost of the unpack only.  The uncached-V4L2-mapping read cost is a
 * property of the SoC's bus and is only real on the target board; numbers from
 * an emulator (qemu-user) are meaningless and must not be quoted.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cam_cr10_unpack.h"

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

typedef void (*row_fn)(const uint8_t *, uint32_t, uint8_t *, unsigned, unsigned);

static double bench(row_fn fn, const uint8_t *src, size_t row_bytes, uint32_t w, uint32_t h,
                    uint8_t *dst, unsigned ob, unsigned sh)
{
	enum { ITERS = 50 };
	double best = 1e30;
	for (int it = 0; it < ITERS; ++it) {
		double t0 = now_ms();
		for (uint32_t y = 0; y < h; ++y) {
			fn(src + row_bytes * y, w, dst + (size_t)y * w * ob, ob, sh);
		}
		double dt = now_ms() - t0;
		if (dt < best) best = dt;
	}
	return best;
}

int main(int argc, char **argv)
{
	uint32_t w  = argc > 1 ? (uint32_t)atoi(argv[1]) : 1456u;
	uint32_t h  = argc > 2 ? (uint32_t)atoi(argv[2]) : 1088u;
	unsigned ob = argc > 3 ? (unsigned)atoi(argv[3]) : 2u;
	unsigned sh = argc > 4 ? (unsigned)atoi(argv[4]) : 0u;
	if (w == 0u || h == 0u || (ob != 1u && ob != 2u)) return 2;

	size_t   row_bytes = (size_t)((w + 5u) / 6u) * 8u;
	uint8_t *src       = malloc(row_bytes * h);
	uint8_t *a         = malloc((size_t)w * h * ob);
	uint8_t *b         = malloc((size_t)w * h * ob);
	if (!src || !a || !b) return 1;
	for (size_t i = 0; i < row_bytes * h; ++i) src[i] = (uint8_t)(i * 2654435761u >> 24);

	double ts = bench(cam_cr10_unpack_row_scalar, src, row_bytes, w, h, a, ob, sh);
	printf("%ux%u out_bytes=%u shift=%u\n", (unsigned)w, (unsigned)h, ob, sh);
	printf("scalar: %.3f ms/frame\n", ts);
#if CAM_CR10_HAVE_NEON
	double tn = bench(cam_cr10_unpack_row_neon, src, row_bytes, w, h, b, ob, sh);
	printf("neon:   %.3f ms/frame (%.2fx)\n", tn, ts / tn);
	if (memcmp(a, b, (size_t)w * h * ob) != 0) {
		puts("MISMATCH neon != scalar");
		return 1;
	}
#else
	puts("neon:   not built (not AArch64)");
#endif
	return 0;
}
