/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-testable half of #2181: sdhc_reset_regs_equal() (examples/aen/
 * aen-sdhc-probe/src/sdhc_reset_capture.h) is a pure struct comparison with
 * no register access, so it can be pinned on native_sim without silicon --
 * unlike sdhc_reset_capture() itself, which reads real DWC SDHC registers
 * and can only run on the E8. These tests fabricate the before/after
 * captures; they say nothing about what a bench run's real registers will
 * show (see the example's PROBE 3 for that).
 */

#include <zephyr/ztest.h>

#include "sdhc_reset_capture.h"

static struct sdhc_reset_regs base_regs(void)
{
	return (struct sdhc_reset_regs){
		.normal_error_int_stat_en   = 0x7effu,
		.normal_error_int_signal_en = 0xffffu,
		.host_ctrl2                 = 0x1008u,
		.pwr_ctrl                   = 0x0fu,
		.clk_ctrl                   = 0x0001u,
	};
}

ZTEST_SUITE(sdhc_reset_regs_compare, NULL, NULL, NULL, NULL, NULL);

ZTEST(sdhc_reset_regs_compare, test_identical_captures_match)
{
	struct sdhc_reset_regs before = base_regs();
	struct sdhc_reset_regs after  = base_regs();

	zassert_true(sdhc_reset_regs_equal(&before, &after),
	             "two captures of the same values must compare equal");
}

/* One field at a time -- pins that every field this driver's reset() is
 * supposed to restore actually participates in the verdict, so a future
 * edit can't silently drop one from the comparison without a test noticing. */
ZTEST(sdhc_reset_regs_compare, test_each_field_alone_flips_the_verdict)
{
	struct sdhc_reset_regs before = base_regs();
	struct sdhc_reset_regs after;

	after = base_regs();
	after.normal_error_int_stat_en ^= 0x1u;
	zassert_false(sdhc_reset_regs_equal(&before, &after), "NORMAL/ERROR_INT_STAT_EN must matter");

	after = base_regs();
	after.normal_error_int_signal_en ^= 0x1u;
	zassert_false(sdhc_reset_regs_equal(&before, &after), "NORMAL/ERROR_INT_SIGNAL_EN must matter");

	after = base_regs();
	after.host_ctrl2 ^= 0x1u;
	zassert_false(sdhc_reset_regs_equal(&before, &after), "HOST_CTRL2 must matter");

	after = base_regs();
	after.pwr_ctrl ^= 0x1u;
	zassert_false(sdhc_reset_regs_equal(&before, &after), "PWR_CTRL must matter");

	after = base_regs();
	after.clk_ctrl ^= 0x1u;
	zassert_false(sdhc_reset_regs_equal(&before, &after), "CLK_CTRL_R must matter");
}

/* The reading #2181 predicts from today's driver: sdhc_dwc_reset() issues
 * SW_RST_ALL only and never re-calls sdhc_dwc_set_def_config(), so a
 * register-file reset that clears these fields to their POR defaults is
 * exactly the MISMATCH this comparison must report, not silently pass. */
ZTEST(sdhc_reset_regs_compare, test_por_default_reset_is_a_mismatch)
{
	struct sdhc_reset_regs before = base_regs();
	struct sdhc_reset_regs after  = { 0 }; /* SW_RST_ALL's POR-default reading, per the DWC spec */

	zassert_false(sdhc_reset_regs_equal(&before, &after),
	              "a register file that reset to all-zero must not read back as restored");
}
