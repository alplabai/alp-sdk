/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #1119: flash_mram_alif.c's flash_range_is_valid() computed
 * `offset + len > FLASH_MRAM_FLASH_SIZE` in unsigned arithmetic -- a large
 * `len` wraps the sum past the flash size and evades the check, letting an
 * out-of-range read/write/erase through to real MRAM. Boundary-tested here,
 * on the host, against the exact flash_mram_range_is_valid() helper the
 * driver now calls.
 */
#include <stddef.h>
#include <sys/types.h>

#include <zephyr/ztest.h>

#include "flash_mram_range.h"

ZTEST_SUITE(flash_mram_range, NULL, NULL, NULL, NULL, NULL);

/* A stand-in for FLASH_MRAM_FLASH_SIZE -- the real value comes from
 * devicetree (DT_REG_SIZE(mram_storage)), which this hosted test has no
 * access to; the bounds-check logic doesn't care what the concrete size is.
 */
#define FAKE_MRAM_SIZE ((size_t)0x00400000u) /* 4 MiB, a plausible MRAM size */

struct range_case {
	const char *name;
	off_t       offset;
	size_t      len;
	size_t      size;
	bool        want;
};

static const struct range_case cases[] = {
	/* Zero length is always valid, anywhere in range (including the end). */
	{ "zero length at start", 0, 0u, FAKE_MRAM_SIZE, true },
	{ "zero length exactly at end", (off_t)FAKE_MRAM_SIZE, 0u, FAKE_MRAM_SIZE, true },

	/* Ordinary in-range request. */
	{ "well within range", 0x100, 0x200u, FAKE_MRAM_SIZE, true },

	/* Exact end boundary. */
	{ "nonzero range ending exactly at size",
	  (off_t)(FAKE_MRAM_SIZE - 0x10u),
	  0x10u,
	  FAKE_MRAM_SIZE,
	  true },

	/* One byte past the end -- must reject. */
	{ "one byte past end", (off_t)(FAKE_MRAM_SIZE - 0x10u), 0x11u, FAKE_MRAM_SIZE, false },
	{ "one-length request one past the end", (off_t)FAKE_MRAM_SIZE, 1u, FAKE_MRAM_SIZE, false },

	/* Negative offset -- must reject before ever reaching the range math. */
	{ "negative offset", -1, 1u, FAKE_MRAM_SIZE, false },
	{ "negative offset, zero len", -1, 0u, FAKE_MRAM_SIZE, false },

	/* The actual #1119 defect: a small nonzero offset with a length near
	 * SIZE_MAX. A buggy `offset + len > size` implementation wraps this
	 * sum to a tiny value and WRONGLY accepts the request -- this is the
	 * exact repro the issue's Evidence section describes. Must reject.
	 */
	{ "small offset, len near SIZE_MAX wraps a naive sum -- must reject",
	  0x10,
	  SIZE_MAX,
	  FAKE_MRAM_SIZE,
	  false },
	{ "offset==size, len==SIZE_MAX-ish -- must reject",
	  (off_t)FAKE_MRAM_SIZE,
	  SIZE_MAX - FAKE_MRAM_SIZE + 1u,
	  FAKE_MRAM_SIZE,
	  false },
};

ZTEST(flash_mram_range, test_range_is_valid_table)
{
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		const struct range_case *tc  = &cases[i];
		bool                     got = flash_mram_range_is_valid(tc->offset, tc->len, tc->size);

		zassert_equal(got, tc->want, "case '%s': got %d want %d", tc->name, got, tc->want);
	}
}

/* Break-test mirroring the issue's own repro: without the subtraction-based
 * rewrite, `(offset + len) > size` wraps for this exact (offset, len) pair
 * and the range is wrongly accepted. This case must PASS (i.e. the range
 * must be rejected) only because the fix is in place.
 */
ZTEST(flash_mram_range, test_wraparound_len_is_rejected)
{
	zassert_false(flash_mram_range_is_valid(0x10, SIZE_MAX, FAKE_MRAM_SIZE),
	              "a wrapping (offset, len) pair must never validate");
}
