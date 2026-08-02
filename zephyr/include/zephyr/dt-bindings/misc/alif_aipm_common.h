/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Devicetree binding constants for the Alif AIPM (Autonomous Intelligent
 * Power Management) run/off profile nodes ("alif,aipm-run" / "alif,aipm-off").
 *
 * hal_alif's se_services/zephyr/src/se_service.c unconditionally does
 * `#include <zephyr/dt-bindings/misc/alif_aipm_common.h>` (needed only when
 * CONFIG_ALIF_SE_DTS_RUN_PROFILE / CONFIG_ALIF_SE_DTS_OFF_PROFILE are on, both
 * `depends on $(dt_nodelabel_enabled,aipm_run/aipm_off)`), but hal_alif does
 * not ship this header -- it exists in neither the pinned Zephyr tree nor
 * hal_alif nor the Apache-2.0 zephyr_alif fork checkout (the fork's
 * dts/bindings/misc/alif,aipm-run.yaml and aipm_ensemble_gen2.dtsi *reference*
 * it, e.g. `#include <dt-bindings/misc/alif_aipm_common.h>`, but the header
 * itself is not vendored in that tree either). Re-authored here from the
 * proprietary Alif DFP (values transcribed, not the source) -- see per-symbol
 * citations below.
 *
 * ADR 0017 Tier-1.5 (vendor-ext dt-bindings header filling a hal_alif
 * packaging gap; no driver logic) -- INTERIM, BENCH-UNVERIFIED: no AIPM
 * run/off profile has been exercised on silicon, and no alp-sdk board yet
 * defines an "alif,aipm-run"/"alif,aipm-off" DT node, so
 * CONFIG_ALIF_SE_DTS_{RUN,OFF}_PROFILE stay off for every current build --
 * this header only needs to exist and be valid for se_service.c to compile.
 *
 * Scope: transcribes ONLY the symbols se_service.c itself references
 * (ALIF_SCALED_FREQ_RC_{ACTIVE,STDBY}_76_8_MHZ, ALIF_IOFLEX_LEVEL_{3V3,1V8}).
 * se_service.c's power-domain default (`PD_SSE700_AON_MASK | PD_SYST_MASK`)
 * uses the UN-prefixed names already defined by hal_alif's own
 * se_services/include/aipm.h (pulled in via se_service.h ->
 * services_lib_api.h), so those are NOT redefined here.
 *
 * NOT transcribed (TBD -- do not guess): the full scaled_clk_freq_t enum
 * (SCALED_FREQ_RC_ACTIVE_{38_4,19_2,9_6,4_8,2_4,1_2,0_6}_MHZ,
 * SCALED_FREQ_RC_STDBY_*, SCALED_FREQ_XO_LOW_DIV_*, and more -- DFP
 * aipm.h:121-168), the ALIF_PD_*_MASK power-domain bitmask family
 * (PD0_MASK..PD9_MASK, PD_VBAT_AON, PD_SRAM_CTRL_AON, PD_SSE700_AON,
 * PD_RTSS_HE, PD_RTSS_HP, PD_SRAMS, PD_SESS, PD_SYST, PD_DBSS -- DFP
 * aipm.h ~40-70), and the per-SoC memory-block masks (ALIF_MRAM_MASK,
 * ALIF_SRAM0_MASK, ALIF_SERAM_MASK, ...) that the fork's
 * alif,aipm-run.yaml says belong in per-SoC headers (alif_aipm_ensemble.h /
 * _gen2.h / _e1c.h / _balletto_b1.h), not this common header. None of these
 * are referenced by any in-tree alp-sdk source or board DT today; add them
 * (with their own DFP citations) only when a real "alif,aipm-run"/
 * "alif,aipm-off" DT node needs them.
 */
#ifndef ALP_DT_BINDINGS_MISC_ALIF_AIPM_COMMON_H_
#define ALP_DT_BINDINGS_MISC_ALIF_AIPM_COMMON_H_

/* scaled_clk_freq_t members se_service.c defaults to when a DTS aipm-run/
 * aipm-off child omits `scaled-clk-freq` / `stby-clk-freq`. Values transcribed
 * from the DFP's scaled_clk_freq_t enum (alif-dfp-ref/se_services/include/
 * aipm.h:121-133); the enum has ~20 more members (see NOT transcribed, above)
 * that no in-tree consumer references yet. */
#define ALIF_SCALED_FREQ_RC_ACTIVE_76_8_MHZ 0 /* aipm.h:122 */
#define ALIF_SCALED_FREQ_RC_STDBY_76_8_MHZ  8 /* aipm.h:132 */

/* ioflex_mode_t (run_profile_t.vdd_ioflex_3V3 / off_profile_t.vdd_ioflex_3V3).
 * Full 2-member enum transcribed verbatim from the DFP
 * (alif-dfp-ref/se_services/include/aipm.h:420-422) -- note the ordering is
 * counter-intuitive (3V3 = 0, the lower-voltage 1V8 = 1); always use the
 * named constant, never a raw integer. */
#define ALIF_IOFLEX_LEVEL_3V3 0 /* aipm.h:421 */
#define ALIF_IOFLEX_LEVEL_1V8 1 /* aipm.h:422 */

#endif /* ALP_DT_BINDINGS_MISC_ALIF_AIPM_COMMON_H_ */
