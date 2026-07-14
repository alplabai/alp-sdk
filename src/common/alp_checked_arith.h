/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared checked-arithmetic helpers (#743).
 *
 * First-party memory/storage/OTA/protocol/MMIO boundary code kept
 * hand-rolling range and narrowing checks such as
 *
 *     if (offset + len > capacity) { ... }
 *
 * or narrowing a size_t to a uint32_t without first proving the value
 * fits.  Both patterns are overflow-prone: the addition itself can
 * wrap before the comparison ever runs, silently accepting an
 * out-of-range request.  This header centralises the safe,
 * subtraction-based pattern the storage backend already documented
 * (src/backends/storage/storage_ops.h) so review can recognise "this
 * boundary check is safe" at a glance instead of re-deriving it per
 * call site.
 *
 * Portable C (no compiler-specific overflow builtin) so the header
 * compiles unmodified in the bare-metal, Zephyr and Yocto trees.
 * Every helper is a `static inline` function -- never a macro -- so
 * each argument is evaluated exactly once and a caller may safely pass
 * an expression with side effects.
 *
 * Internal only: NOT part of the public Alp ABI.  Do not add a public
 * alp/ header re-exporting this one -- see `securing-the-alp-sdk-position`
 * (portable surface stays vendor- and helper-clean) and `alp_errno.h`
 * for the precedent of a private, static-inline-only header living in
 * src/common/.
 */

#ifndef ALP_COMMON_ALP_CHECKED_ARITH_H_
#define ALP_COMMON_ALP_CHECKED_ARITH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Overflow-safe range check: is [@p offset, @p offset + @p len)
 *        wholly inside [0, @p capacity)?
 *
 * Never computes `offset + len` -- that sum can wrap before a naive
 * `offset + len > capacity` comparison runs, which would let an
 * out-of-range request through.  Headroom is instead computed by
 * subtraction only after `offset <= capacity` is already proven, so
 * the subtraction itself cannot wrap.
 *
 * `offset == capacity` is accepted ONLY when `len == 0` (a
 * zero-length request exactly at the end of the region is valid; any
 * non-zero length starting there is not).
 *
 * @param offset    Start of the requested range.
 * @param len       Length of the requested range, in the same unit as
 *                  @p offset / @p capacity.
 * @param capacity  Size of the region the range must fit inside.
 * @return true iff the range lies wholly within [0, capacity).
 */
static inline bool alp_size_range_valid(size_t offset, size_t len, size_t capacity)
{
	if (offset > capacity) {
		return false;
	}
	/* offset <= capacity here, so capacity - offset cannot wrap. */
	return len <= capacity - offset;
}

/**
 * @brief Narrow a @p value to a `uint32_t`, rejecting anything that
 *        would not round-trip.
 *
 * @param[in]  value  Value to narrow.
 * @param[out] out    Set to the narrowed value on success.  MUST NOT be
 *                     NULL.  Left UNCHANGED when this function returns
 *                     false -- callers may pre-seed `*out` with a
 *                     sentinel and rely on it surviving a rejected
 *                     conversion.
 * @return true iff @p value <= UINT32_MAX (i.e. @p value fits in a
 *         `uint32_t` without truncation), in which case `*out` holds
 *         the narrowed value.
 */
static inline bool alp_size_to_u32(size_t value, uint32_t *out)
{
	/* On a target where size_t is already 32-bit (every Cortex-M
	 * build this SDK ships to today), `value > UINT32_MAX` is a
	 * compile-time-false comparison that -Wtype-limits /
	 * -Wtautological-constant-out-of-range-compare flags under the
	 * strict-warnings + -Werror profile (issue #634).  Guard the
	 * comparison out entirely on those targets -- SIZE_MAX <=
	 * UINT32_MAX there already proves every size_t value fits, so
	 * the check only needs to exist where size_t is wider (native
	 * host builds, native_sim, a mocked test). */
#if SIZE_MAX > UINT32_MAX
	if (value > (size_t)UINT32_MAX) {
		return false;
	}
#endif
	*out = (uint32_t)value;
	return true;
}

/**
 * @brief Checked `uint32_t` addition: @p a + @p b without wraparound.
 *
 * @param[in]  a    First addend.
 * @param[in]  b    Second addend.
 * @param[out] out  Set to `a + b` on success.  MUST NOT be NULL.  Left
 *                   UNCHANGED when this function returns false.
 * @return true iff `a + b` is representable as a `uint32_t` (does not
 *         overflow), in which case `*out` holds the sum.
 */
static inline bool alp_u32_add_checked(uint32_t a, uint32_t b, uint32_t *out)
{
	if (b > UINT32_MAX - a) {
		return false;
	}
	*out = a + b;
	return true;
}

/**
 * @brief Checked `uint64_t` addition: @p a + @p b without wraparound.
 *
 * 64-bit sibling of @ref alp_u32_add_checked for the boundaries (e.g.
 * <alp/storage.h>'s 64-bit offset/length API) that carry values wider
 * than a `uint32_t` on every target, including a 32-bit `size_t` one.
 *
 * @param[in]  a    First addend.
 * @param[in]  b    Second addend.
 * @param[out] out  Set to `a + b` on success.  MUST NOT be NULL.  Left
 *                   UNCHANGED when this function returns false.
 * @return true iff `a + b` is representable as a `uint64_t` (does not
 *         overflow), in which case `*out` holds the sum.
 */
static inline bool alp_u64_add_checked(uint64_t a, uint64_t b, uint64_t *out)
{
	if (b > UINT64_MAX - a) {
		return false;
	}
	*out = a + b;
	return true;
}

/**
 * @brief `uint64_t` sibling of @ref alp_size_range_valid.
 *
 * Same "never compute offset + len, offset == capacity valid only for
 * len == 0" contract, sized for boundaries whose offset/length are
 * always 64-bit regardless of the host `size_t` width (e.g.
 * <alp/storage.h>'s public read/write/erase API).
 *
 * @param offset    Start of the requested range.
 * @param len       Length of the requested range.
 * @param capacity  Size of the region the range must fit inside.
 * @return true iff the range lies wholly within [0, capacity).
 */
static inline bool alp_u64_range_valid(uint64_t offset, uint64_t len, uint64_t capacity)
{
	if (offset > capacity) {
		return false;
	}
	return len <= capacity - offset;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_COMMON_ALP_CHECKED_ARITH_H_ */
