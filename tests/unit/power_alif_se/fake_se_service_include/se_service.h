/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-only stand-in for hal_alif's <se_service.h>, used ONLY by this test
 * directory to compile the REAL src/backends/power/alif_se_power.c on native_sim.
 *
 * Unlike the trimmed fake of power_alif_dcdc_bounds, off_profile_t here carries
 * EVERY member of hal_alif's aipm.h in the same order (v2.3.0, se_services/
 * include/aipm.h): the backend's job is to assign each of them, and a test that
 * cannot see a member cannot prove it was assigned.  alif_se_power.c static-asserts
 * that vtor_address_ns is the last member, so a hal_alif bump that appends one
 * fails the real build, and the enum ordinals below are copied verbatim.
 */

#ifndef ALP_TEST_FAKE_SE_SERVICE_H
#define ALP_TEST_FAKE_SE_SERVICE_H

#include <stdint.h>

/* aipm.h: Power Domains */
#define PD0_MASK (1 << 0)
#define PD2_MASK (1 << 2)

#define PD_VBAT_AON_MASK   PD0_MASK /* bit0 */
#define PD_SSE700_AON_MASK PD2_MASK /* bit2 */

/* aipm.h: LF Clock Sources */
typedef enum {
	CLK_SRC_LFRC = 0,
	CLK_SRC_LFXO,
} lfclock_t;

/* aipm.h: HF Clock Sources */
typedef enum {
	CLK_SRC_HFRC = 0,
	CLK_SRC_HFXO,
	CLK_SRC_PLL
} hfclock_t;

/* aipm.h: Scaled HFRC/HFXO clock frequencies -- ordinals matter. */
typedef enum {
	SCALED_FREQ_RC_ACTIVE_76_8_MHZ = 0,
	SCALED_FREQ_RC_ACTIVE_38_4_MHZ,
	SCALED_FREQ_RC_ACTIVE_19_2_MHZ,
	SCALED_FREQ_RC_ACTIVE_9_6_MHZ,
	SCALED_FREQ_RC_ACTIVE_4_8_MHZ,
	SCALED_FREQ_RC_ACTIVE_2_4_MHZ,
	SCALED_FREQ_RC_ACTIVE_1_2_MHZ,
	SCALED_FREQ_RC_ACTIVE_0_6_MHZ,

	SCALED_FREQ_RC_STDBY_76_8_MHZ = 8,
	SCALED_FREQ_RC_STDBY_38_4_MHZ,
	SCALED_FREQ_RC_STDBY_19_2_MHZ,
	SCALED_FREQ_RC_STDBY_4_8_MHZ,
	SCALED_FREQ_RC_STDBY_1_2_MHZ,
	SCALED_FREQ_RC_STDBY_0_6_MHZ,
	SCALED_FREQ_RC_STDBY_0_3_MHZ,
	SCALED_FREQ_RC_STDBY_0_075_MHZ,

	SCALED_FREQ_XO_LOW_DIV_38_4_MHZ = 16,
	SCALED_FREQ_XO_LOW_DIV_19_2_MHZ,
	SCALED_FREQ_XO_LOW_DIV_9_6_MHZ,
	SCALED_FREQ_XO_LOW_DIV_4_8_MHZ,
	SCALED_FREQ_XO_LOW_DIV_2_4_MHZ,
	SCALED_FREQ_XO_LOW_DIV_1_2_MHZ,
	SCALED_FREQ_XO_LOW_DIV_0_6_MHZ,
	SCALED_FREQ_XO_LOW_DIV_0_3_MHZ,

	SCALED_FREQ_XO_HIGH_DIV_38_4_MHZ = 24,
	SCALED_FREQ_XO_HIGH_DIV_19_2_MHZ,
	SCALED_FREQ_XO_HIGH_DIV_9_6_MHZ,
	SCALED_FREQ_XO_HIGH_DIV_2_4_MHZ,
	SCALED_FREQ_XO_HIGH_DIV_0_6_MHZ,
	SCALED_FREQ_XO_HIGH_DIV_0_3_MHZ,
	SCALED_FREQ_XO_HIGH_DIV_0_15_MHZ,
	SCALED_FREQ_XO_HIGH_DIV_0_0375_MHZ,
	SCALED_FREQ_NONE,
} scaled_clk_freq_t;

/* aipm.h */
typedef enum {
	DCDC_MODE_OFF = 0,
	DCDC_MODE_PFM_AUTO,
	DCDC_MODE_PFM_FORCED,
	DCDC_MODE_PWM
} dcdc_mode_t;

/* aipm.h */
typedef enum {
	IOFLEX_LEVEL_3V3,
	IOFLEX_LEVEL_1V8
} ioflex_mode_t;

/* aipm.h: the OFF profile, every member in declaration order. */
typedef struct {
	uint32_t          power_domains;
	uint32_t          dcdc_voltage;
	dcdc_mode_t       dcdc_mode;
	lfclock_t         aon_clk_src;
	hfclock_t         stby_clk_src;
	scaled_clk_freq_t stby_clk_freq;
	uint32_t          memory_blocks;
	uint32_t          ip_clock_gating;
	uint32_t          phy_pwr_gating;
	ioflex_mode_t     vdd_ioflex_3V3;
	uint32_t          wakeup_events;
	uint32_t          ewic_cfg;
	uint32_t          vtor_address;
	uint32_t          vtor_address_ns;
} off_profile_t;

int se_service_get_off_cfg(off_profile_t *wp);
int se_service_set_off_cfg(off_profile_t *wp);

#endif /* ALP_TEST_FAKE_SE_SERVICE_H */
