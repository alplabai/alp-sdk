/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #2784: the gen2 aiPM bit layout in alif_aipm_gen2.h.  The real
 * guard is the header's _Static_asserts, which fire at compile time; this
 * file includes it and pins the one value whose gen1/gen2 mix-up hurts
 * (BACKUP4K = bit21, not bit20) at run time too, so a regression shows up
 * as a named failing test and not only a build error.
 */

#include <zephyr/ztest.h>

#include "alif_aipm_gen2.h"

ZTEST(power_alif_aipm_gen2, test_backup4k_is_bit21_not_fwram)
{
	zassert_equal(ALP_AIPM_GEN2_BACKUP4K_MASK, 0x00200000u, "BACKUP4K must be bit21");
	zassert_equal(ALP_AIPM_GEN2_FWRAM_MASK, 0x00100000u, "FWRAM must be bit20");
}

ZTEST(power_alif_aipm_gen2, test_wake_masks)
{
	zassert_equal(ALP_AIPM_GEN2_WE_LPTIMER, 0xF00u, "LPTIMER wake bits");
	zassert_equal(ALP_AIPM_GEN2_WE_LPGPIO, 0xFF0000u, "LPGPIO wake bits");
}

ZTEST_SUITE(power_alif_aipm_gen2, NULL, NULL, NULL, NULL, NULL);
