/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-only stand-in for hal_alif's <se_service.h>, used ONLY by this test
 * directory to compile the REAL src/backends/power/alif_se_profile.c
 * translation unit on native_sim, where the real header (and the SE
 * hardware it talks to) does not exist.
 *
 * Field/ordinal layout copied from modules/hal/alif/se_services/include/
 * aipm.h (run_profile_t / off_profile_t / clock_frequency_t /
 * scaled_clk_freq_t / hfclock_t / ioflex_mode_t) -- trimmed to exactly the
 * fields and enumerators alif_se_profile.c references, in the same order,
 * so a hal_alif bump that reorders or renames one of these fails this
 * fake's compile instead of silently mismatching the real ABI.
 */

#ifndef ALP_TEST_FAKE_SE_SERVICE_H
#define ALP_TEST_FAKE_SE_SERVICE_H

#include <stdint.h>

/* aipm.h: HF Clock Sources */
typedef enum {
	CLK_SRC_HFRC = 0,
	CLK_SRC_HFXO,
	CLK_SRC_PLL,
} hfclock_t;

/* aipm.h: Clocks frequencies */
typedef enum {
	CLOCK_FREQUENCY_800MHZ,
	CLOCK_FREQUENCY_400MHZ,
	CLOCK_FREQUENCY_300MHZ,
	CLOCK_FREQUENCY_200MHZ,
	CLOCK_FREQUENCY_160MHZ,
	CLOCK_FREQUENCY_120MHZ,
	CLOCK_FREQUENCY_80MHZ,
	CLOCK_FREQUENCY_60MHZ,
	CLOCK_FREQUENCY_100MHZ,
	CLOCK_FREQUENCY_50MHZ,
	CLOCK_FREQUENCY_20MHZ,
	CLOCK_FREQUENCY_10MHZ,
	CLOCK_FREQUENCY_76_8_RC_MHZ,
	CLOCK_FREQUENCY_38_4_RC_MHZ,
	CLOCK_FREQUENCY_76_8_XO_MHZ,
	CLOCK_FREQUENCY_38_4_XO_MHZ,
	CLOCK_FREQUENCY_DISABLED,
} clock_frequency_t;

/* aipm.h: Scaled HFRC/HFXO clock frequencies -- ordinals matter (the
 * vendor enum is non-monotonic), copied verbatim. */
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
	IOFLEX_LEVEL_3V3,
	IOFLEX_LEVEL_1V8,
} ioflex_mode_t;

/* aipm.h: Power Management Data Structures -- trimmed to the fields
 * alif_se_profile.c reads/writes. */
typedef struct {
	uint32_t          power_domains;
	uint32_t          dcdc_voltage;
	hfclock_t         run_clk_src;
	clock_frequency_t cpu_clk_freq;
	scaled_clk_freq_t scaled_clk_freq;
	uint32_t          memory_blocks;
	ioflex_mode_t     vdd_ioflex_3V3;
} run_profile_t;

typedef struct {
	uint32_t          power_domains;
	uint32_t          dcdc_voltage;
	hfclock_t         stby_clk_src;
	scaled_clk_freq_t stby_clk_freq;
	uint32_t          memory_blocks;
	ioflex_mode_t     vdd_ioflex_3V3;
	uint32_t          wakeup_events;
} off_profile_t;

int se_service_get_run_cfg(run_profile_t *pp);
int se_service_set_run_cfg(run_profile_t *pp);
int se_service_get_off_cfg(off_profile_t *wp);
int se_service_set_off_cfg(off_profile_t *wp);

#endif /* ALP_TEST_FAKE_SE_SERVICE_H */
