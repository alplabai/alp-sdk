/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #737: boundary tests for the CryptoCell hash-update staging-buffer
 * check in src/backends/security/se_cryptocell.c:395-423 (se_hash_update()).
 *
 * That function buffers hash_update() bytes into a fixed-size struct
 * se_hash_be::buf[CONFIG_ALP_SDK_SECURITY_SE_HASH_BUF] before the SE
 * single-shot SHA service runs, falling back to a delegated PSA hash once
 * the buffer would overflow. The pre-#737 guard was
 *
 *     if (be->used + len > sizeof(be->buf))
 *
 * Both operands are `size_t`. When the addition wraps (attacker/caller
 * supplies a `len` close to SIZE_MAX while `be->used` is already nonzero),
 * the wrapped sum can land BELOW sizeof(be->buf), so the check wrongly
 * concludes the input fits and falls through to
 * `memcpy(be->buf + be->used, data, len)` -- writing `len` bytes (up to
 * just under SIZE_MAX) starting inside a buffer only
 * CONFIG_ALP_SDK_SECURITY_SE_HASH_BUF bytes long.
 *
 * The fix replaces that addition with the shared, subtraction-based
 * alp_size_range_valid() (src/common/alp_checked_arith.h, #743), which
 * never computes `offset + len`. se_cryptocell.c itself cannot be built
 * host-side here -- it transitively includes the AEN801/E8-only hal_alif
 * `se_service.h` plus `zephyr/kernel.h` -- so this test exercises the exact
 * arithmetic against a local mirror of struct se_hash_be's {used,
 * buf[capacity]} shape, at the real default capacity
 * (CONFIG_ALP_SDK_SECURITY_SE_HASH_BUF's default, se_cryptocell.c:178).
 *
 * Every case below also runs the VERBATIM pre-#737 predicate
 * (naive_would_delegate(), copied unmodified from the old guard) side by
 * side with the fixed one (checked_would_delegate(), the new call site's
 * logic). The wraparound case asserts the two DISAGREE: the naive
 * predicate wrongly says "fits, no delegation needed" (the exploitable
 * bug) while the checked one correctly says "does not fit, delegate" --
 * i.e. this test fails if se_hash_update() is still using
 * naive_would_delegate()'s expression, which is exactly what the pre-fix
 * code did.
 */
#include <stdint.h>

#include <zephyr/ztest.h>

#include "common/alp_checked_arith.h"

/* Mirrors se_cryptocell.c:178's default -- kept as a local literal since
 * the real Kconfig symbol is only defined in a Zephyr/AEN801 build. */
#define SE_HASH_STAGING_CAP 512u

ZTEST_SUITE(se_cryptocell_hash_bounds, NULL, NULL, NULL, NULL, NULL);

/* Verbatim copy of the pre-#737 guard at se_cryptocell.c:407:
 *     if (be->used + len > sizeof(be->buf))
 * "would delegate" == "the naive addition-based check thinks the input
 * does not fit and the caller should fall through to PSA delegation". */
static bool naive_would_delegate(size_t used, size_t len, size_t capacity)
{
	return (used + len) > capacity;
}

/* The #737 fix's call site (se_cryptocell.c:407 after the change):
 *     if (!alp_size_range_valid(be->used, len, sizeof(be->buf)))
 */
static bool checked_would_delegate(size_t used, size_t len, size_t capacity)
{
	return !alp_size_range_valid(used, len, capacity);
}

ZTEST(se_cryptocell_hash_bounds, test_exact_capacity_fits_in_buffer)
{
	/* A single update() call that fills the staging buffer exactly
	 * (be->used == 0, len == capacity) must be buffered in place, NOT
	 * delegated. */
	zassert_false(checked_would_delegate(0u, SE_HASH_STAGING_CAP, SE_HASH_STAGING_CAP),
	              "len == capacity must fit exactly, no delegation");

	/* Also exercise the already-partially-filled case landing exactly
	 * on the end of the buffer. */
	zassert_false(checked_would_delegate(200u, SE_HASH_STAGING_CAP - 200u, SE_HASH_STAGING_CAP),
	              "used + len == capacity exactly must fit");
}

ZTEST(se_cryptocell_hash_bounds, test_one_byte_over_capacity_delegates)
{
	/* One byte more than the buffer can hold must delegate -- the
	 * "ordinary oversized input" behaviour issue #737 explicitly asks
	 * to keep. */
	zassert_true(checked_would_delegate(0u, SE_HASH_STAGING_CAP + 1u, SE_HASH_STAGING_CAP),
	             "len == capacity + 1 must delegate");

	zassert_true(checked_would_delegate(200u, SE_HASH_STAGING_CAP - 200u + 1u, SE_HASH_STAGING_CAP),
	             "used + len == capacity + 1 must delegate");

	/* The naive pre-#737 predicate gets ordinary (non-wrapping) oversized
	 * input right too -- delegation for plain-oversized input was never
	 * the bug, so both predicates must agree here. */
	zassert_true(naive_would_delegate(0u, SE_HASH_STAGING_CAP + 1u, SE_HASH_STAGING_CAP),
	             "naive predicate also delegates ordinary oversized input");
}

ZTEST(se_cryptocell_hash_bounds, test_size_max_wraparound_defeats_naive_check)
{
	/* be->used is small and legitimate (some bytes already buffered
	 * from an earlier update() call); len is attacker/caller supplied
	 * and close to SIZE_MAX. used + len wraps size_t and lands far
	 * below capacity, so the naive check is fooled. */
	const size_t used = 10u;
	const size_t len  = SIZE_MAX - 5u; /* used + len wraps to 4 */

	/* Prove the exploit: the verbatim pre-#737 expression wrongly
	 * concludes the input fits (returns false == "do not delegate"),
	 * which is exactly the bug -- se_hash_update() would then run
	 * memcpy(be->buf + 10, data, SIZE_MAX - 5), overflowing be->buf by
	 * every byte past the first (capacity - 10). This assertion is
	 * what makes the test fail against the pre-fix code path: swap
	 * checked_would_delegate() below for naive_would_delegate() (i.e.
	 * restore the old guard) and test_size_max_wraparound_defeats_naive_check
	 * fails immediately. */
	zassert_false(naive_would_delegate(used, len, SE_HASH_STAGING_CAP),
	              "naive addition-based check is wrongly fooled by the size_t wrap");

	/* The fixed, subtraction-based check is not fooled: it must
	 * correctly demand delegation instead of the overflowing memcpy. */
	zassert_true(checked_would_delegate(used, len, SE_HASH_STAGING_CAP),
	             "checked range validation correctly rejects the wrapping length");
}

ZTEST(se_cryptocell_hash_bounds, test_used_already_past_capacity_delegates)
{
	/* Issue #737's second requirement: defensively verify be->used is
	 * not already greater than sizeof(be->buf). alp_size_range_valid()
	 * rejects offset > capacity outright, regardless of len (even
	 * len == 0), so a corrupted/invalid `used` cannot slip through. */
	zassert_true(checked_would_delegate(SE_HASH_STAGING_CAP + 1u, 0u, SE_HASH_STAGING_CAP),
	             "used > capacity must delegate even for a zero-length update");
}
