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
#include "alp/cap_instance.h"
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
     * can do (runtime gate -- pairs with ALP_HAS() from <alp/cap.h>
     * for the compile-time SoC-level gate).
     *
     * For HW oversampling: this is reachable through the portable
     * config field cfg->oversampling_ratio at open time -- no vendor
     * ext needed.  Re-open with oversampling_ratio=8 to demonstrate. */
	const alp_capabilities_t *caps = alp_adc_capabilities(adc);
	if (alp_capabilities_has(caps, ALP_INSTANCE_CAP_HW_OVERSAMPLE)) {
		printf("[adc] backend advertises HW oversampling -- "
		       "set cfg.oversampling_ratio at open time to enable\n");
	} else {
		printf("[adc] no HW oversampling on this build\n");
	}

	int32_t      uv = 0;
	alp_status_t s  = alp_adc_read_uv(adc, &uv);
	printf("[adc] read_uv -> status=%d, uv=%d\n", (int)s, (int)uv);

	alp_adc_close(adc);
	printf("[adc] done\n");
	return 0;
}
