/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * sdhc_reset_capture -- the register snapshot this app takes around
 * sdhc_hw_reset() to answer #2181: does the DWC SDHC driver's reset() path
 * actually restore the configuration sdhc_dwc_set_def_config() programs at
 * init, or does the bare SW_RST_ALL it issues leave those registers at
 * their post-reset silicon defaults?
 *
 * sdhc_dwc_reset() (this repo's zephyr/drivers/sdhc/sdhc_dwc.c, the ALP-SDK
 * delta over upstream) issues `sdhc_dwc_hw_reset(dev,
 * DWC_SDHC_SW_RST_ALL_Msk)` and then, as of #2122, re-calls
 * sdhc_dwc_set_def_config() unconditionally -- even on a reset timeout --
 * so a MATCH is the expected reading of today's code. What #2122 fixed is
 * the register-programming half; whether that re-programming actually
 * lands and survives on real silicon (register write ordering, timing,
 * clock-gate races the source can't show) is exactly what is unmeasured.
 * This header only captures and compares; it draws no conclusion a bench
 * run hasn't produced.
 *
 * Split in two on purpose:
 *   - sdhc_reset_capture()    reads real silicon (sys_read32) -- lives in
 *     main.c, Zephyr-only, never built on host.
 *   - sdhc_reset_regs_equal() pure struct comparison, no I/O at all -- host
 *     testable under native_sim (tests/unit/sdhc_reset_regs_compare/), which
 *     includes this header directly, same pattern
 *     tests/unit/rail_features/ uses for examples/ai/rail-predictive-
 *     maintenance/src/rail_features.c.
 *
 * sdhc_reset_regs_equal() alone cannot catch a silently-broken
 * set_def_config(): `sdhc_dwc_init()` (POST_KERNEL, before main() ever
 * runs) already calls `sdhc_dwc_reset()` once, so main.c's "before" capture
 * is ALSO a post-set_def_config() reading, not a pre-reset one. If
 * set_def_config() were a no-op on some silicon, both captures would read
 * POR and still compare equal. sdhc_reset_regs_matches_def_config() checks
 * the after-capture against the fixed values set_def_config() itself
 * programs, independent of any earlier capture -- see main.c, PROBE 3.
 */

#ifndef SDHC_RESET_CAPTURE_H_
#define SDHC_RESET_CAPTURE_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * The seven registers #2181 names as unverified across reset() (addresses
 * per the issue, SD_REG_BASE = 0x48102000):
 *   NORMAL_INT_STAT_EN    0x034 (16b) -- packed with ERROR_INT_STAT_EN
 *   ERROR_INT_STAT_EN     0x036 (16b) -- into one aligned 32b read
 *   NORMAL_INT_SIGNAL_EN  0x038 (16b) -- packed with ERROR_INT_SIGNAL_EN
 *   ERROR_INT_SIGNAL_EN   0x03a (16b) -- into one aligned 32b read
 *   HOST_CTRL2            0x03e (16b)
 *   PWR_CTRL              0x029 (8b)
 *   CLK_CTRL_R            0x02c (16b)
 */
struct sdhc_reset_regs {
	uint32_t normal_error_int_stat_en;   /* 0x034 low16 / 0x036 high16 */
	uint32_t normal_error_int_signal_en; /* 0x038 low16 / 0x03a high16 */
	uint16_t host_ctrl2;                 /* 0x03e */
	uint8_t  pwr_ctrl;                   /* 0x029 */
	uint16_t clk_ctrl;                   /* 0x02c */
};

/*
 * Pure comparison -- no register access, no Zephyr dependency. Returns true
 * iff every field this app captured matches. Field-by-field, not memcmp:
 * struct padding is implementation-defined and must never flip the verdict.
 */
static inline bool sdhc_reset_regs_equal(const struct sdhc_reset_regs *before,
                                         const struct sdhc_reset_regs *after)
{
	return before->normal_error_int_stat_en == after->normal_error_int_stat_en &&
	       before->normal_error_int_signal_en == after->normal_error_int_signal_en &&
	       before->host_ctrl2 == after->host_ctrl2 && before->pwr_ctrl == after->pwr_ctrl &&
	       before->clk_ctrl == after->clk_ctrl;
}

/*
 * The fixed values `sdhc_dwc_set_def_config()` (zephyr/drivers/sdhc/
 * sdhc_dwc.c) itself programs -- all POR-zero, so a capture reading POR on
 * every one of these bits means set_def_config() never ran or never landed,
 * regardless of what any earlier capture shows:
 *   - NORMAL_INT_STAT_EN / ERROR_INT_STAT_EN: ORed with NORM_INTR_ALL_Msk /
 *     ERROR_INTR_ALL_Msk (0xFFFF each, less the card IRQ bit) -- non-zero.
 *   - NORMAL_INT_SIGNAL_EN / ERROR_INT_SIGNAL_EN: ORed with CC|TC|DMA|BWR|
 *     BRR / ERROR_INTR_ALL_Msk -- non-zero.
 *   - HOST_CTRL2: ASYNC_INT_EN (bit14) | VER4_EN (bit12) set.
 *   - PWR_CTRL: bit0 (VDD1 bus power) set by set_power(SDHC_POWER_ON).
 *   - CLK_CTRL_R: bit0 (INTERNAL_CLK_EN) and bit2 (CLK_EN) set by
 *     sdhc_dwc_clock_set() once the clock reports stable -- bit1 (the
 *     STABLE status flag) is already masked out of the capture, see
 *     main.c's SD_CLK_CTRL_STABLE_BIT.
 */
#define SDHC_RESET_CAPTURE_HOST_CTRL2_EXPECT_Msk 0x5000u /* ASYNC_INT_EN | VER4_EN */
#define SDHC_RESET_CAPTURE_PWR_CTRL_VDD1_Msk     0x01u
#define SDHC_RESET_CAPTURE_CLK_CTRL_EXPECT_Msk   0x05u /* INTERNAL_CLK_EN | CLK_EN */

static inline bool sdhc_reset_regs_matches_def_config(const struct sdhc_reset_regs *r)
{
	return (r->normal_error_int_stat_en & 0xFFFFu) != 0u &&
	       (r->normal_error_int_stat_en >> 16) != 0u &&
	       (r->normal_error_int_signal_en & 0xFFFFu) != 0u &&
	       (r->normal_error_int_signal_en >> 16) != 0u &&
	       (r->host_ctrl2 & SDHC_RESET_CAPTURE_HOST_CTRL2_EXPECT_Msk) ==
	           SDHC_RESET_CAPTURE_HOST_CTRL2_EXPECT_Msk &&
	       (r->pwr_ctrl & SDHC_RESET_CAPTURE_PWR_CTRL_VDD1_Msk) != 0u &&
	       (r->clk_ctrl & SDHC_RESET_CAPTURE_CLK_CTRL_EXPECT_Msk) ==
	           SDHC_RESET_CAPTURE_CLK_CTRL_EXPECT_Msk;
}

#endif /* SDHC_RESET_CAPTURE_H_ */
