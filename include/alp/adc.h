/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file adc.h
 * @brief ALP SDK analog-to-digital converter abstraction.
 *
 * One-shot ADC reads against studio-resolved channel indices.  The
 * channel-config side (resolution, gain, reference, acquisition time)
 * lives in devicetree on the Zephyr backend; this header only exposes
 * the runtime knobs apps actually tune.
 *
 * Backends:
 *   - Zephyr   : `adc_*` driver class with `adc_sequence` framing.
 *   - Yocto    : Industrial I/O sysfs (`/sys/bus/iio/devices/`).
 *   - Baremetal: vendor HAL ADC sequencer.
 *
 * Typical usage:
 * @code
 *     alp_adc_t *th = alp_adc_open(&(alp_adc_config_t){
 *         .channel_id = 0,
 *         .resolution_bits = 12,
 *         .reference  = ALP_ADC_REF_INTERNAL,
 *     });
 *     int32_t uv = 0;
 *     alp_adc_read_uv(th, &uv);   // 0..3300000 µV typical
 * @endcode
 */

#ifndef ALP_ADC_H
#define ALP_ADC_H

#include <stdint.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Reference voltage source. */
typedef enum {
    ALP_ADC_REF_INTERNAL   = 0,    /**< On-die bandgap reference. */
    ALP_ADC_REF_EXTERNAL_0 = 1,    /**< External pin pair 0 (VREF+ / VREF-). */
    ALP_ADC_REF_EXTERNAL_1 = 2,    /**< Secondary external reference where present. */
    ALP_ADC_REF_VDD        = 3     /**< Use VDD as the reference. */
} alp_adc_ref_t;

/** Opaque ADC channel handle.  Allocate via @ref alp_adc_open. */
typedef struct alp_adc alp_adc_t;

/** Configuration passed to @ref alp_adc_open. */
typedef struct {
    uint32_t      channel_id;       /**< Studio-resolved ADC channel index (0..7). */
    uint8_t       resolution_bits;  /**< 8 / 10 / 12 / 14 / 16 typical. 0 = use DT default. */
    uint16_t      acquisition_us;   /**< Sample-and-hold time, microseconds. */
    alp_adc_ref_t reference;
    uint8_t       gain_num;         /**< Gain numerator (e.g. 1 for 1/1). */
    uint8_t       gain_den;         /**< Gain denominator (e.g. 6 for 1/6). */
} alp_adc_config_t;

/**
 * @brief Acquire and configure an ADC channel.
 *
 * Resolves @p cfg->channel_id via the `alp-adc<N>` devicetree alias,
 * applies the channel-config registers, and returns a handle ready
 * for one-shot reads.  Continuous-sampling and DMA streaming arrive
 * in v0.3 as a separate API.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL; @c channel_id < 8.
 * @return Open handle on success, or NULL if the channel can't be
 *         resolved, configured, or the pool is exhausted.
 */
alp_adc_t   *alp_adc_open(const alp_adc_config_t *cfg);

/**
 * @brief One-shot read returning the raw conversion result.
 *
 * The raw value is the ADC's native code in the configured
 * resolution.  No reference / gain scaling is applied — use
 * @ref alp_adc_read_uv if you want microvolts.
 *
 * @param[in]  adc      Handle from @ref alp_adc_open.
 * @param[out] raw_out  Receives the raw code.  Sign-extended on
 *                      ADCs with differential inputs.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t alp_adc_read_raw(alp_adc_t *adc, int32_t *raw_out);

/**
 * @brief One-shot read converted to microvolts.
 *
 * Applies the configured reference voltage and gain to the raw code.
 * Convertible to engineering units by the caller via the sensor's
 * transfer function.
 *
 * @param[in]  adc     Handle from @ref alp_adc_open.
 * @param[out] uv_out  Receives the signed microvolt reading.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t alp_adc_read_uv(alp_adc_t *adc, int32_t *uv_out);

/**
 * @brief Release an ADC handle back to the pool.
 *
 * Does not power-down the underlying converter; that's a SoC-wide
 * decision the studio's power-management layer handles.  NULL is a
 * no-op.
 *
 * @param[in] adc  Handle from @ref alp_adc_open, or NULL.
 */
void         alp_adc_close(alp_adc_t *adc);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_ADC_H */
