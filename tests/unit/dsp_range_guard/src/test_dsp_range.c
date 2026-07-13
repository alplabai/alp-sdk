/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #734: the CMSIS-DSP stats path narrows a size_t element count to the
 * uint32_t kernel block size.  alp_dsp_cmsis_block_len() is the guard that
 * rejects a count too large to represent so alp_dsp_stats_f32 returns
 * ALP_ERR_OUT_OF_RANGE instead of letting CMSIS silently truncate and
 * process a prefix.  Boundary-tested here on a wide-size_t host.
 */
#include <stdint.h>

#include <zephyr/ztest.h>

#include "dsp_range.h"

ZTEST_SUITE(dsp_range_guard, NULL, NULL, NULL, NULL, NULL);

ZTEST(dsp_range_guard, test_representable_len_narrows_exactly)
{
	uint32_t block = 0xFFFFFFFFu;
	zassert_true(alp_dsp_cmsis_block_len(0u, &block), "0 is representable");
	zassert_equal(block, 0u, NULL);
	zassert_true(alp_dsp_cmsis_block_len(1234u, &block), "small count representable");
	zassert_equal(block, 1234u, "narrowed exactly");
}

ZTEST(dsp_range_guard, test_uint32_max_boundary)
{
	uint32_t block = 0u;
	/* UINT32_MAX is the largest representable block -> accepted, exact. */
	zassert_true(alp_dsp_cmsis_block_len((size_t)UINT32_MAX, &block),
	             "UINT32_MAX must be accepted");
	zassert_equal(block, UINT32_MAX, "narrowed exactly, no truncation");

#if SIZE_MAX > UINT32_MAX
	/* One past the uint32_t ceiling -- only expressible where size_t is
	 * wider (native_sim/native/64).  Must be rejected BEFORE any kernel
	 * runs so the CMSIS path can't process a truncated prefix (#734). */
	block = 0xA5A5A5A5u; /* sentinel: must stay untouched on reject */
	zassert_false(alp_dsp_cmsis_block_len((size_t)UINT32_MAX + 1u, &block),
	              "UINT32_MAX+1 must be rejected");
	zassert_equal(block, 0xA5A5A5A5u, "block_out left untouched on reject");
#endif
}
