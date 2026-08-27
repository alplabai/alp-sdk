/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #1648 tier 1: the Alif E7 / E8 ADC backends' open() converted the
 * portable `oversampling_ratio` (a RATIO) to Zephyr's adc_sequence.
 * oversampling (a log2 EXPONENT) via `__builtin_ctz()`, which
 * silently rounds a non-power-of-two ratio DOWN instead of failing
 * (3x and 6x both landed on 1x / 2x, ALP_OK either way). Exercises
 * the guard, alp_adc_oversampling_ratio_ok(), that now refuses those
 * ratios with ALP_ERR_NOSUPPORT before __builtin_ctz() ever runs --
 * on the host, no devicetree / MMIO / real ADC involved.
 */
#include <zephyr/ztest.h>

#include "adc_oversampling.h"

ZTEST_SUITE(adc_oversampling, NULL, NULL, NULL, NULL, NULL);

/* 0 and 1 both mean "no oversampling" and must always be accepted. */
ZTEST(adc_oversampling, test_zero_and_one_accepted)
{
	zassert_true(alp_adc_oversampling_ratio_ok(0u), "0 (backend default) must be accepted");
	zassert_true(alp_adc_oversampling_ratio_ok(1u), "1x (no oversampling) must be accepted");
}

/* Every power of two up to the documented ceiling (256) must be
 * accepted -- these are exactly the ratios __builtin_ctz() can map
 * to an exact log2 exponent.
 */
ZTEST(adc_oversampling, test_powers_of_two_accepted)
{
	static const uint16_t ratios[] = { 2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u };

	for (size_t i = 0; i < ARRAY_SIZE(ratios); ++i) {
		zassert_true(alp_adc_oversampling_ratio_ok(ratios[i]),
		             "%u is a power of two and must be accepted",
		             ratios[i]);
	}
}

/* Non-power-of-two ratios -- the ones __builtin_ctz() would silently
 * round down (issue #1648) -- must be refused.
 */
ZTEST(adc_oversampling, test_non_powers_of_two_refused)
{
	static const uint16_t ratios[] = { 3u, 5u, 6u, 7u, 9u, 100u, 255u };

	for (size_t i = 0; i < ARRAY_SIZE(ratios); ++i) {
		zassert_false(alp_adc_oversampling_ratio_ok(ratios[i]),
		              "%u is not a power of two and must be refused",
		              ratios[i]);
	}
}

/* Powers of two above the documented 1..256 ceiling must be refused too --
 * without an upper bound, 512/1024/... pass the bare power-of-two test
 * (`ratio & (ratio - 1) == 0`) despite sitting above every ratio
 * <alp/adc.h> documents as supported.
 */
ZTEST(adc_oversampling, test_powers_of_two_above_ceiling_refused)
{
	static const uint16_t ratios[] = { 512u, 1024u, 32768u };

	for (size_t i = 0; i < ARRAY_SIZE(ratios); ++i) {
		zassert_false(alp_adc_oversampling_ratio_ok(ratios[i]),
		              "%u is a power of two but above the 256 ceiling and must be refused",
		              ratios[i]);
	}
}
