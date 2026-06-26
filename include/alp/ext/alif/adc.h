/**
 * @file ext/alif/adc.h
 * @brief Alif Ensemble ADC vendor-specific knobs.
 *
 * Non-portable.  Include only when you've committed to Alif
 * silicon for the gated feature.  Every function in this header
 * verifies the handle's backend is Alif before touching hardware;
 * calls on a non-Alif handle return
 * @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC.
 *
 * @par Supported silicon: alif:ensemble:e7
 *      (e3 / e5 land in a follow-up.  e8 silicon-verified in v0.8.0: 15/17 aen-* apps PASS on real E8.)
 *
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      First vendor-extension header.  Promotes to [ABI-STABLE]
 *      when three vendor families ship extensions.
 */

#ifndef ALP_EXT_ALIF_ADC_H
#define ALP_EXT_ALIF_ADC_H

#include <stdint.h>

#include <alp/adc.h>
#include <alp/peripheral.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Compile-time presence marker — used by example code to gate vendor calls. */
#define ALP_EXT_ALIF_ADC_AVAILABLE 1

/** Trigger source for the Alif ADC hardware sequencer. */
typedef enum {
	ALP_ALIF_ADC_TRIGGER_SOFTWARE = 0,
	ALP_ALIF_ADC_TRIGGER_TIMER0,
	ALP_ALIF_ADC_TRIGGER_TIMER1,
	ALP_ALIF_ADC_TRIGGER_TIMER2,
	ALP_ALIF_ADC_TRIGGER_TIMER3,
	ALP_ALIF_ADC_TRIGGER_EXT_PIN,
} alp_alif_adc_trigger_t;

/* Note: HW oversampling is NOT a vendor extension -- it's reachable
 * through the portable @ref alp_adc_config_t::oversampling_ratio field
 * at open time, which the Alif backend translates into Zephyr's
 * adc_sequence.oversampling.  Adding alp_alif_adc_set_oversampling()
 * would duplicate functionality the portable surface already covers.
 * Per the vendor-ext audit rule: extensions are reserved for surface
 * that the portable layer (or Zephyr's adc_*) genuinely cannot reach. */

/**
 * @brief Configure Alif ADC hardware trigger source.
 *
 * @par Supported silicon: alif:ensemble:e7
 *
 * @param h    Handle from @ref alp_adc_open opened against an Alif SoC.
 * @param src  Trigger source from @ref alp_alif_adc_trigger_t.
 * @return  @ref ALP_OK / @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC /
 *          @ref ALP_ERR_INVAL.
 */
alp_status_t alp_alif_adc_set_trigger_source(alp_adc_t *h, alp_alif_adc_trigger_t src);

#ifdef __cplusplus
}
#endif

#endif /* ALP_EXT_ALIF_ADC_H */
