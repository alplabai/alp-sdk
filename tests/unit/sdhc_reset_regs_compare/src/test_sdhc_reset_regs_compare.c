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

/* A guard against a silently-wrong comparator: if two captures differ, a
 * changed field must not compare equal. This is NOT what PROBE 3's real
 * before-capture would see -- on real hardware the "before" capture is
 * itself post-sdhc_dwc_init()'s own reset (see sdhc_reset_capture.h), so a
 * set_def_config() that silently no-ops would leave BOTH captures at POR
 * and sdhc_reset_regs_equal() would report MATCH; catching that case is
 * sdhc_reset_regs_matches_def_config()'s job, tested below. */
ZTEST(sdhc_reset_regs_compare, test_por_default_reset_is_a_mismatch)
{
	struct sdhc_reset_regs before = base_regs();
	struct sdhc_reset_regs after  = { 0 }; /* SW_RST_ALL's POR-default reading, per the DWC spec */

	zassert_false(sdhc_reset_regs_equal(&before, &after),
	              "a register file that reset to all-zero must not read back as restored");
}

/*
 * sdhc_reset_regs_matches_def_config() is what actually catches the
 * silently-no-op case above: a POR-all-zero capture must fail it (nothing
 * set_def_config() programs is ever zero), and the values it genuinely
 * writes (see sdhc_reset_capture.h's comment) must pass it, even though
 * they never equal an identical "before" capture in the same test.
 */
ZTEST(sdhc_reset_regs_compare, test_por_default_does_not_match_def_config)
{
	struct sdhc_reset_regs after = { 0 };

	zassert_false(sdhc_reset_regs_matches_def_config(&after),
	              "an all-POR capture must not read back as set_def_config() having run");
}

ZTEST(sdhc_reset_regs_compare, test_def_config_values_match)
{
	struct sdhc_reset_regs after = {
		.normal_error_int_stat_en   = 0xfffffeffu,
		.normal_error_int_signal_en = 0xffff003bu,
		.host_ctrl2                 = 0x5000u,
		.pwr_ctrl                   = 0x0fu,
		.clk_ctrl                   = 0x0005u,
	};

	zassert_true(sdhc_reset_regs_matches_def_config(&after),
	             "the values sdhc_dwc_set_def_config() actually programs must pass");
}
