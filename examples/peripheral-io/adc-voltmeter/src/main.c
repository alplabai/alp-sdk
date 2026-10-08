/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * adc-voltmeter — read EVK_ADC_ARDUINO_A1 and print the result in µV.
 * Demonstrates the capability-validation contract by also trying
 * a deliberately-too-high resolution and showing the rejection.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/adc.h"
#include "alp/cap.h" /* alp_has() / ALP_HAS(); also pulls in cap_instance.h */
#include "alp/peripheral.h"
#include "alp/boards/alp_e1m_evk_routes.h"

/* Defensive include: the Alif vendor-extension header only exists when
 * the build sees CONFIG_ALP_SOC_ALIF_ENSEMBLE_E8 (or similar).  __has_include
 * lets the example compile on every SoM without per-SoC #ifdefs. */
#ifdef __has_include
#if __has_include(<alp/ext/alif/adc.h>)
#include <alp/ext/alif/adc.h>
#endif
#endif

int main(void)
{
	/* Bring up the SDK runtime before anything else -- thin today,
	 * but future backends rely on it (see <alp/peripheral.h>). */
	(void)alp_init();

	/* 0. Capability gate -- ask the silicon, not the board name.
	 *    alp_has() reads the generated capability table for the active
	 *    CONFIG_ALP_SOC_<...>, so the same source runs on every SoM with
	 *    no #ifdef CONFIG_BOARD_* forks.  With no SoC selected
	 *    (native_sim) the capability layer is permissive and the demo
	 *    proceeds, relying on open() failing gracefully instead.
	 *    ALP_HAS(HW_ADC) is the compile-time twin when the unused branch
	 *    should be dropped from the binary entirely. */
	if (!alp_has(ALP_CAP_ID_HW_ADC)) {
		printf("[adc] no ADC on this SoC (%s) -- skipping\n", ALP_SOC_REF_STR);
		printf("[adc] done\n");
		return 0;
	}

	/* 1. Capability rejection: ask for an absurd resolution.
     *    With any concrete CONFIG_ALP_SOC_<...>=y this returns NULL
     *    with last_error = ALP_ERR_OUT_OF_RANGE.  With ALP_SOC_NONE
     *    (default) the macros are UINT16_MAX so the check passes
     *    through and we land on NOT_READY instead. */
	printf("[adc] capability check: requesting 100-bit resolution\n");
	alp_adc_t *bad = alp_adc_open(&(alp_adc_config_t){
	    .channel_id      = EVK_ADC_ARDUINO_A1,
	    .resolution_bits = 100, /* deliberately unreasonable */
	    .reference       = ALP_ADC_REF_INTERNAL,
	});
	if (bad == NULL) {
		printf("[adc] rejected: alp_last_error=%d (expected -8 OUT_OF_RANGE "
		       "or -2 NOT_READY)\n",
		       (int)alp_last_error());
	} else {
		alp_adc_close(bad);
	}

	/* 2. Real read at 12-bit resolution. */
	printf("[adc] open EVK_ADC_ARDUINO_A1 @ 12 bits\n");
	alp_adc_t *adc = alp_adc_open(&(alp_adc_config_t){
	    .channel_id      = EVK_ADC_ARDUINO_A1,
	    .resolution_bits = 12,
	    .reference       = ALP_ADC_REF_INTERNAL,
	});
	if (adc == NULL) {
		printf("[adc] open failed: alp_last_error=%d "
		       "(expected NOT_READY = -2 on native_sim — no ADC controller)\n",
		       (int)alp_last_error());
		printf("[adc] done\n");
		return 0;
	}

	/* 3. Capability-gated teaching block.
     *
     * `alp_adc_capabilities` asks the backend what THIS opened handle
     * can do (instance-level runtime gate -- pairs with the SoC-level
     * alp_has() / ALP_HAS() gate demonstrated at step 0).
     *
     * Two-word design (<alp/cap_instance.h>): `flags` carries the
     * universal ALP_INSTANCE_CAP_* bits, `class_flags` carries the
     * ADC-owned ALP_ADC_CAP_* bits (<alp/adc.h>) -- oversampling is an
     * ADC fact, not a universal one, so it lives in class_flags and is
     * only meaningful once ALP_INSTANCE_CAP_REPORTED is set in flags
     * (an unset REPORTED means the backend never spoke at all).
     *
     * No ADC backend in this tree advertises ALP_ADC_CAP_HW_OVERSAMPLE
     * (#1648): the vendored Alif driver rejects every non-zero
     * adc_sequence.oversampling outright, so alif_e7 / alif_e8 refuse any
     * cfg.oversampling_ratio > 1 at alp_adc_open time with
     * ALP_ERR_NOSUPPORT instead of advertising a capability they can't
     * honour.  This branch is dead on every SoM this example targets --
     * it stays only so the pattern is visible for a future backend that
     * does support HW oversampling. */
	const alp_capabilities_t *caps = alp_adc_capabilities(adc);
	if (alp_capabilities_has(caps, ALP_INSTANCE_CAP_REPORTED) &&
	    (caps->class_flags & ALP_ADC_CAP_HW_OVERSAMPLE) != 0u) {
		printf("[adc] backend advertises HW oversampling -- "
		       "set cfg.oversampling_ratio at open time to enable\n");
	} else {
		printf("[adc] no HW oversampling reported on this build\n");
	}

	int32_t      uv = 0;
	alp_status_t s  = alp_adc_read_uv(adc, &uv);
	printf("[adc] read_uv -> status=%d, uv=%d\n", (int)s, (int)uv);

	alp_adc_close(adc);
	printf("[adc] done\n");
	return 0;
}
