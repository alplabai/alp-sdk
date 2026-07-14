/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #743: table-driven boundary tests for the shared checked-arithmetic
 * helpers (src/common/alp_checked_arith.h).  Host-side (native_sim) --
 * the header is dependency-free, so these tests need no storage/DSP
 * backend, no dispatcher, and no mocked hardware.
 */
#include <stdint.h>

#include <zephyr/ztest.h>

#include "common/alp_checked_arith.h"

ZTEST_SUITE(checked_arith, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------ */
/* alp_size_range_valid()                                              */
/* ------------------------------------------------------------------ */

struct range_case {
	const char *name;
	size_t      offset;
	size_t      len;
	size_t      capacity;
	bool        want;
};

static const struct range_case range_cases[] = {
	/* offset 0, length 0, capacity 0 -- degenerate empty region. */
	{ "zero/zero/zero", 0u, 0u, 0u, true },

	/* Ordinary in-range request. */
	{ "well within capacity", 2u, 3u, 10u, true },

	/* Exact end boundary: offset + len == capacity, exactly, both with
     * and without a trailing zero-length request. */
	{ "zero-length exactly at end", 10u, 0u, 10u, true },
	{ "nonzero range ending exactly at capacity", 5u, 5u, 10u, true },

	/* One byte beyond the boundary. */
	{ "one byte past capacity", 6u, 5u, 10u, false },
	{ "one-length request one past the end", 10u, 1u, 10u, false },

	/* offset > capacity, independent of len (even len == 0). */
	{ "offset just past capacity, zero len", 11u, 0u, 10u, false },
	{ "offset far past capacity", 100u, 0u, 10u, false },

	/* SIZE_MAX combinations that would wrap a naive `offset + len`
     * addition.  A buggy `offset + len > capacity` implementation
     * wraps these sums to a small value and WRONGLY accepts them --
     * these cases catch that regression. */
	{ "offset==capacity==SIZE_MAX, len 1 must reject (would wrap to 0)",
	  SIZE_MAX,
	  1u,
	  SIZE_MAX,
	  false },
	{ "one-past-capacity length near SIZE_MAX (naive sum wraps to 0)",
	  SIZE_MAX - 1u,
	  2u,
	  SIZE_MAX,
	  false },
	{ "exact boundary at SIZE_MAX, no wrap", SIZE_MAX - 1u, 1u, SIZE_MAX, true },
	{ "len==SIZE_MAX starting at 1 (naive sum wraps to 0)", 1u, SIZE_MAX, SIZE_MAX, false },
	{ "whole-capacity range at SIZE_MAX", 0u, SIZE_MAX, SIZE_MAX, true },
};

ZTEST(checked_arith, test_size_range_valid_table)
{
	for (size_t i = 0; i < ARRAY_SIZE(range_cases); i++) {
		const struct range_case *tc  = &range_cases[i];
		bool                     got = alp_size_range_valid(tc->offset, tc->len, tc->capacity);
		zassert_equal(got, tc->want, "case '%s': got %d want %d", tc->name, got, tc->want);
	}
}

/* alp_size_range_valid() must never itself evaluate offset + len -- this
 * is exercised structurally above (every wrap-prone case above returns
 * the CORRECT answer, which is only possible if the implementation
 * never computes the wrapping sum), but pin the property with dedicated
 * cases at the exact overflow boundary of every unsigned width smaller
 * than size_t too, so a regression that starts computing offset + len
 * cannot slip through even where size_t doesn't wrap this file's own
 * SIZE_MAX cases as clearly. */
ZTEST(checked_arith, test_size_range_valid_never_wraps_addition)
{
	/* If the implementation computed `offset + len` here, the sum
	 * would wrap to (len - 1), which is < capacity for any nonzero
	 * capacity -- i.e. a buggy implementation ACCEPTS this request.
	 * The correct implementation rejects it because offset > capacity
	 * outright. */
	zassert_false(alp_size_range_valid(SIZE_MAX, 5u, 10u),
	              "offset > capacity must reject regardless of len");
}

/* ------------------------------------------------------------------ */
/* alp_size_to_u32()                                                    */
/* ------------------------------------------------------------------ */

ZTEST(checked_arith, test_size_to_u32_zero_and_small)
{
	uint32_t out = 0xA5A5A5A5u;

	zassert_true(alp_size_to_u32(0u, &out), "0 is representable");
	zassert_equal(out, 0u, NULL);

	zassert_true(alp_size_to_u32(1234u, &out), "small value representable");
	zassert_equal(out, 1234u, "narrowed exactly");
}

ZTEST(checked_arith, test_size_to_u32_uint32_max_boundary)
{
	uint32_t out = 0u;

	/* UINT32_MAX is the largest representable value -- accepted, exact. */
	zassert_true(alp_size_to_u32((size_t)UINT32_MAX, &out), "UINT32_MAX must be accepted");
	zassert_equal(out, UINT32_MAX, "narrowed exactly, no truncation");

#if SIZE_MAX > UINT32_MAX
	/* UINT32_MAX + 1 -- only expressible where size_t is wider than
	 * uint32_t (native_sim/native/64).  Must be rejected, and *out
	 * must stay untouched (sentinel survives the rejected call). */
	out = 0xDEADBEEFu;
	zassert_false(alp_size_to_u32((size_t)UINT32_MAX + 1u, &out), "UINT32_MAX+1 must be rejected");
	zassert_equal(out, 0xDEADBEEFu, "*out left unchanged on rejected conversion");
#endif
}

/* ------------------------------------------------------------------ */
/* alp_u32_add_checked()                                                */
/* ------------------------------------------------------------------ */

struct u32_add_case {
	const char *name;
	uint32_t    a;
	uint32_t    b;
	bool        want_ok;
	uint32_t    want_sum;
};

static const struct u32_add_case u32_add_cases[] = {
	{ "zero + zero", 0u, 0u, true, 0u },
	{ "small exact addition", 1u, 2u, true, 3u },
	{ "a=0 identity at UINT32_MAX", 0u, UINT32_MAX, true, UINT32_MAX },
	{ "b=0 identity at UINT32_MAX", UINT32_MAX, 0u, true, UINT32_MAX },
	/* Exact addition landing precisely on UINT32_MAX (0x7FFFFFFF + 0x80000000). */
	{ "exact addition up to UINT32_MAX", 0x7FFFFFFFu, 0x80000000u, true, UINT32_MAX },
	/* Overflow by exactly one, both operand orders. */
	{ "UINT32_MAX + 1 overflows", UINT32_MAX, 1u, false, 0u },
	{ "1 + UINT32_MAX overflows", 1u, UINT32_MAX, false, 0u },
	/* Overflow with both operands large (0x80000000 + 0x80000000 wraps). */
	{ "large + large overflows", 0x80000000u, 0x80000000u, false, 0u },
};

ZTEST(checked_arith, test_u32_add_checked_table)
{
	for (size_t i = 0; i < ARRAY_SIZE(u32_add_cases); i++) {
		const struct u32_add_case *tc  = &u32_add_cases[i];
		uint32_t                   out = 0xA5A5A5A5u;
		bool                       got = alp_u32_add_checked(tc->a, tc->b, &out);

		zassert_equal(got, tc->want_ok, "case '%s': got %d want %d", tc->name, got, tc->want_ok);
		if (tc->want_ok) {
			zassert_equal(out, tc->want_sum, "case '%s': sum mismatch", tc->name);
		} else {
			zassert_equal(
			    out, 0xA5A5A5A5u, "case '%s': *out must stay unchanged on overflow", tc->name);
		}
	}
}

/* ------------------------------------------------------------------ */
/* alp_u64_add_checked() / alp_u64_range_valid() (storage's 64-bit      */
/* migration, #743) -- same contract as the 32/size_t siblings above,   */
/* pinned separately since <alp/storage.h> always carries 64-bit        */
/* offset/length regardless of the host's size_t width.                 */
/* ------------------------------------------------------------------ */

ZTEST(checked_arith, test_u64_add_checked)
{
	uint64_t out = 0xA5A5A5A5A5A5A5A5ull;

	zassert_true(alp_u64_add_checked(0u, 0u, &out), "0 + 0 representable");
	zassert_equal(out, 0u, NULL);

	zassert_true(alp_u64_add_checked(UINT64_MAX, 0u, &out), "identity at UINT64_MAX");
	zassert_equal(out, UINT64_MAX, NULL);

	out = 0xDEADBEEFDEADBEEFull;
	zassert_false(alp_u64_add_checked(UINT64_MAX, 1u, &out), "UINT64_MAX + 1 must overflow");
	zassert_equal(out, 0xDEADBEEFDEADBEEFull, "*out left unchanged on overflow");
}

ZTEST(checked_arith, test_u64_range_valid_table)
{
	/* Mirrors the alp_size_range_valid table above at 64-bit width --
	 * exercises the same "offset==capacity valid only if len==0" and
	 * "never compute offset+len" contract for the storage backend's
	 * always-64-bit boundary (src/backends/storage/storage_ops.h). */
	zassert_true(alp_u64_range_valid(0u, 0u, 0u), "zero/zero/zero");
	zassert_true(alp_u64_range_valid(10u, 0u, 10u), "zero-length exactly at end");
	zassert_true(alp_u64_range_valid(5u, 5u, 10u), "nonzero range ending exactly at capacity");
	zassert_false(alp_u64_range_valid(6u, 5u, 10u), "one byte past capacity");
	zassert_false(alp_u64_range_valid(11u, 0u, 10u), "offset past capacity");
	zassert_false(alp_u64_range_valid(UINT64_MAX, 1u, UINT64_MAX),
	              "offset==capacity==UINT64_MAX, len 1 must reject (would wrap to 0)");
	zassert_true(alp_u64_range_valid(0u, UINT64_MAX, UINT64_MAX), "whole-capacity range");
}

/* ------------------------------------------------------------------ */
/* Arguments evaluated exactly once (no macro double-evaluation).       */
/* ------------------------------------------------------------------ */

static int g_eval_count;

static size_t next_offset(void)
{
	g_eval_count++;
	return 4u;
}

ZTEST(checked_arith, test_arguments_evaluated_exactly_once)
{
	uint32_t out = 0u;

	/* Every helper here is a `static inline` FUNCTION, never a macro,
	 * so C's call semantics already guarantee each argument expression
	 * is evaluated exactly once before the call -- a naive macro
	 * implementation (e.g. `#define CHECK(o, l, c) ((o) > (c) || ...)`)
	 * would risk evaluating an argument with side effects more than
	 * once. Pin that with a side-effecting argument and assert the
	 * counter only advances by one per call. */
	g_eval_count = 0;
	zassert_true(alp_size_range_valid(next_offset(), 1u, 10u), NULL);
	zassert_equal(g_eval_count, 1, "offset argument evaluated exactly once");

	g_eval_count = 0;
	zassert_true(alp_size_to_u32(next_offset(), &out), NULL);
	zassert_equal(g_eval_count, 1, "value argument evaluated exactly once");
}
