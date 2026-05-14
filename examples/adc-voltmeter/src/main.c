/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * adc-voltmeter — read E1M_ADC0 and print the result in µV.
 * Demonstrates the capability-validation contract by also trying
 * a deliberately-too-high resolution and showing the rejection.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/adc.h"
#include "alp/e1m_pinout.h"

int main(void) {
    /* 1. Capability rejection: ask for an absurd resolution.
     *    With any concrete CONFIG_ALP_SOC_<...>=y this returns NULL
     *    with last_error = ALP_ERR_OUT_OF_RANGE.  With ALP_SOC_NONE
     *    (default) the macros are UINT16_MAX so the check passes
     *    through and we land on NOT_READY instead. */
    printf("[adc] capability check: requesting 100-bit resolution\n");
    alp_adc_t *bad = alp_adc_open(&(alp_adc_config_t){
        .channel_id      = E1M_ADC0,
        .resolution_bits = 100,        /* deliberately unreasonable */
        .reference       = ALP_ADC_REF_INTERNAL,
    });
    if (bad == NULL) {
        printf("[adc] rejected: alp_last_error=%d (expected -8 OUT_OF_RANGE "
               "or -2 NOT_READY)\n", (int)alp_last_error());
    } else {
        alp_adc_close(bad);
    }

    /* 2. Real read at 12-bit resolution. */
    printf("[adc] open E1M_ADC0 @ 12 bits\n");
    alp_adc_t *adc = alp_adc_open(&(alp_adc_config_t){
        .channel_id      = E1M_ADC0,
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

    int32_t uv = 0;
    alp_status_t s = alp_adc_read_uv(adc, &uv);
    printf("[adc] read_uv -> status=%d, uv=%d\n", (int)s, (int)uv);

    alp_adc_close(adc);
    printf("[adc] done\n");
    return 0;
}
