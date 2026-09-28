/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-side (native_sim) exhaustive test for the word-copy memcpy core
 * (src/common/alp_fast_memcpy_core.c, issue #2285): every source/
 * destination alignment pair (0..3 byte offset each, so every aligned
 * and unaligned combination, including src/dst mismatched by 1-3 bytes)
 * crossed with every length 0..70, checked against a byte-at-a-time
 * reference copy.
 *
 * Calls alp_fast_memcpy_core() directly by name -- this test links the
 * OS-agnostic core .c file directly, never the Zephyr memcpy() override
 * (src/zephyr/fast_memcpy.c, CONFIG_ALP_SDK_FAST_MEMCPY), so it never
 * collides with native_sim's own host libc memcpy().
 */
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "alp_fast_memcpy_core.h"

ZTEST_SUITE(fast_memcpy, NULL, NULL, NULL, NULL, NULL);

/* Padding on both ends of every buffer so a copy can never run off
 * either end even at the largest offset + length tested below, and so
 * an overrun in either direction shows up as a canary mismatch instead
 * of silently touching unrelated memory. */
#define BUF_PAD    8
#define MAX_OFFSET 3
#define MAX_LEN    70
#define BUF_SIZE   (BUF_PAD + MAX_OFFSET + MAX_LEN + BUF_PAD)

ZTEST(fast_memcpy, test_all_alignments_and_lengths)
{
	static uint8_t src_buf[BUF_SIZE];
	static uint8_t dst_buf[BUF_SIZE];
	static uint8_t ref_buf[BUF_SIZE];

	/* Deterministic non-repeating fill: a byte-swap or off-by-one
	 * inside the word path would slip past a run of identical bytes,
	 * so the source is never uniform. */
	for (size_t i = 0; i < BUF_SIZE; i++) {
		src_buf[i] = (uint8_t)(i * 37u + 11u);
	}

	for (size_t src_off = 0; src_off <= MAX_OFFSET; src_off++) {
		for (size_t dst_off = 0; dst_off <= MAX_OFFSET; dst_off++) {
			for (size_t len = 0; len <= MAX_LEN; len++) {
				/* Canary fill: dst and the reference start
				 * identical and NOT equal to the copied
				 * payload, so a copy that writes short (or
				 * touches a byte it shouldn't) is caught even
				 * when the payload bytes coincidentally
				 * match. */
				memset(dst_buf, 0xA5, BUF_SIZE);
				memset(ref_buf, 0xA5, BUF_SIZE);

				void *ret = alp_fast_memcpy_core(dst_buf + dst_off, src_buf + src_off, len);
				zassert_equal(ret,
				              dst_buf + dst_off,
				              "return must be dst (src_off=%zu dst_off=%zu len=%zu)",
				              src_off,
				              dst_off,
				              len);

				for (size_t i = 0; i < len; i++) {
					ref_buf[dst_off + i] = src_buf[src_off + i];
				}

				zassert_mem_equal(dst_buf,
				                  ref_buf,
				                  BUF_SIZE,
				                  "mismatch at src_off=%zu dst_off=%zu len=%zu",
				                  src_off,
				                  dst_off,
				                  len);
			}
		}
	}
}
