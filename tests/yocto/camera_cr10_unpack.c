/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file camera_cr10_unpack.c
 * @brief CR10 unpack: AArch64 NEON == scalar == an independent packer, bit for bit (#2792).
 *
 * Sweeps odd widths (1..130 plus real sensor widths), row strides with and
 * without CRU padding, shifts 0/2 and both output sizes.  Each source row
 * ends against a PROT_NONE page so a read past ceil(w/6)*8 bytes faults, and
 * the destination is canary-fenced so a store past w*out_bytes is caught.
 * On a non-AArch64 build the NEON path does not exist; the dispatcher is the
 * scalar one and the test still checks it against the independent packer.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cam_cr10_unpack.h"
#include "test_assert.h"

static uint32_t g_rng = 0x12345678u;

static uint32_t rnd(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	return g_rng;
}

/* Independent packer: pixel x of a row at bit 10*(x%6) of word x/6. */
static void pack_px(uint8_t *row, uint32_t x, uint32_t v)
{
	uint64_t word;
	memcpy(&word, row + (size_t)(x / 6u) * 8u, 8u);
	word |= (uint64_t)(v & 0x3FFu) << ((x % 6u) * 10u);
	memcpy(row + (size_t)(x / 6u) * 8u, &word, 8u);
}

#define CANARY 0xC3u
#define FENCE  32u

static int check(uint32_t w, uint32_t pad_words, unsigned out_bytes, unsigned shift)
{
	enum { H = 3 };
	size_t   row_bytes = (size_t)((w + 5u) / 6u) * 8u;
	size_t   stride    = row_bytes + (size_t)pad_words * 8u;
	long     pg        = sysconf(_SC_PAGESIZE);
	size_t   len       = ((stride * H + (size_t)pg - 1u) / (size_t)pg) * (size_t)pg;
	uint8_t *m =
	    mmap(NULL, len + (size_t)pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m == MAP_FAILED || mprotect(m + len, (size_t)pg, PROT_NONE) != 0) return 0;
	/* the last row's data ends exactly at the guard page */
	uint8_t *src = m + len - (stride * (H - 1u) + row_bytes);

	size_t   npx = (size_t)w * H;
	uint8_t *want = malloc(npx * out_bytes);
	uint8_t *got  = malloc(npx * out_bytes + 2u * FENCE);
	uint8_t *dst  = got + FENCE;
	int      ok   = want != NULL && got != NULL;
	if (!ok) return 0;
	memset(got, CANARY, npx * out_bytes + 2u * FENCE);

	for (uint32_t y = 0; y < H; ++y) {
		uint8_t *row = src + stride * y;
		for (size_t i = 0; i < row_bytes; ++i) row[i] = 0;
		/* padding between rows is junk the unpacker must never read into the result */
		if (y + 1u < H) {
			for (size_t i = row_bytes; i < stride; ++i) row[i] = (uint8_t)rnd();
		}
		for (uint32_t x = 0; x < w; ++x) {
			uint32_t v = rnd() & 0x3FFu;
			pack_px(row, x, v);
			uint32_t s = v >> shift;
			size_t   o = ((size_t)y * w + x) * out_bytes;
			want[o]    = (uint8_t)s;
			if (out_bytes == 2u) want[o + 1] = (uint8_t)(s >> 8);
		}
		/* bits 60..63 of every word, and the unused pixel slots of a partial last word,
		 * are junk the unpacker must ignore */
		for (size_t off = 0; off < row_bytes; off += 8u) {
			uint64_t wd;
			memcpy(&wd, row + off, 8u);
			wd |= 0xF000000000000000ull;
			if (off + 8u == row_bytes && w % 6u != 0u) {
				wd |= (((uint64_t)rnd() << 32) | rnd()) << ((w % 6u) * 10u);
			}
			memcpy(row + off, &wd, 8u);
		}
	}

	for (int variant = 0; variant < 1 + CAM_CR10_HAVE_NEON; ++variant) {
		memset(dst, 0, npx * out_bytes);
		for (uint32_t y = 0; y < H; ++y) {
			uint8_t *d = dst + (size_t)y * w * out_bytes;
			const uint8_t *r = src + stride * y;
			if (variant == 0) {
				cam_cr10_unpack_row_scalar(r, w, d, out_bytes, shift);
			} else {
#if CAM_CR10_HAVE_NEON
				cam_cr10_unpack_row_neon(r, w, d, out_bytes, shift);
#endif
			}
		}
		ok = ok && memcmp(dst, want, npx * out_bytes) == 0;
		for (unsigned i = 0; i < FENCE; ++i) {
			ok = ok && got[i] == CANARY && dst[npx * out_bytes + i] == CANARY;
		}
		if (!ok) {
			fprintf(stderr, "FAIL w=%u pad=%u out=%u shift=%u variant=%d\n", (unsigned)w,
			        (unsigned)pad_words, out_bytes, shift, variant);
			break;
		}
		memset(got, CANARY, npx * out_bytes + 2u * FENCE);
	}
	free(want);
	free(got);
	munmap(m, len + (size_t)pg);
	return ok;
}

int main(void)
{
	static const uint32_t big[] = { 191, 192, 193, 255, 256, 640, 1280, 1455, 1456, 1457, 1920 };
	unsigned              cases = 0;
	for (uint32_t i = 0; i < 130u + sizeof(big) / sizeof(big[0]); ++i) {
		uint32_t w = i < 130u ? i + 1u : big[i - 130u];
		/* pad 0 = tight packing, 3 = CRU-style padding, w = the real width*8 stride */
		uint32_t pads[] = { 0u, 3u, w };
		for (unsigned p = 0; p < 3u; ++p) {
			for (unsigned ob = 1u; ob <= 2u; ++ob) {
				for (unsigned sh = 0u; sh <= 2u; sh += 2u) {
					ALP_ASSERT_TRUE(check(w, pads[p], ob, sh));
					++cases;
				}
			}
		}
	}
	printf("camera_cr10_unpack: %u cases, neon=%d\n", cases, CAM_CR10_HAVE_NEON);
	ALP_TEST_SUMMARY();
}
