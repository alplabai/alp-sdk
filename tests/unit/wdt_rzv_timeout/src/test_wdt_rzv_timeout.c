/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RZ/V2N WDT timeout selection (zephyr/drivers/watchdog/wdt_rzv_timeout.h).
 * The expected CKS/TOPS encodings are typed from the hardware manual
 * (R01UH1071EJ0120, 5.4.2.2.2 / Table 5.4-3), not read back from the table.
 */

#include <zephyr/ztest.h>

#include "wdt_rzv_timeout.h"

#define CLK_24MHZ 24000000U

ZTEST(wdt_rzv_timeout, test_longest_period_not_above_request)
{
	uint8_t tops;
	uint8_t cks;

	/* 24 MHz /256 x 16384 = 174762 us -> 174 ms fits, so 175 ms is the cap that selects it. */
	zassert_true(wdt_rzv_pick(CLK_24MHZ, 175, &tops, &cks));
	zassert_equal(tops, 3);
	zassert_equal(cks, 0x5);

	/* 1 ms: the longest period under it is /1 x 16384 = 682 us (CKS 0000b, TOPS 11b). */
	zassert_true(wdt_rzv_pick(CLK_24MHZ, 1, &tops, &cks));
	zassert_equal(tops, 3);
	zassert_equal(cks, 0x0);
}

ZTEST(wdt_rzv_timeout, test_only_manual_dividers_are_ever_chosen)
{
	static const uint8_t allowed[] = { 0x0, 0x2, 0x3, 0x4, 0xF, 0x5 };

	for (uint32_t ms = 1; ms < 4000; ms += 7) {
		uint8_t tops;
		uint8_t cks;

		if (!wdt_rzv_pick(CLK_24MHZ, ms, &tops, &cks)) {
			continue;
		}
		bool ok = false;

		for (size_t i = 0; i < sizeof(allowed); i++) {
			ok = ok || (cks == allowed[i]);
		}
		zassert_true(ok, "CKS 0x%x is prohibited (ms=%u)", cks, ms);
		zassert_true(tops <= 3);
	}
}

ZTEST(wdt_rzv_timeout, test_too_short_and_bad_clock)
{
	uint8_t tops;
	uint8_t cks;

	/* 0 ms never fits, and a zero clock must not divide by zero. */
	zassert_false(wdt_rzv_pick(CLK_24MHZ, 0, &tops, &cks));
	zassert_false(wdt_rzv_pick(0, 1000, &tops, &cks));
	/* a slow clock whose shortest period (1024 / 1 kHz = 1.024 s) exceeds the request */
	zassert_false(wdt_rzv_pick(1000, 1000, &tops, &cks));
}

ZTEST_SUITE(wdt_rzv_timeout, NULL, NULL, NULL, NULL, NULL);
