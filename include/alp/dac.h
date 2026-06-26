/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dac.h
 * @brief Alp SDK digital-to-analog converter abstraction.
 *
 * DAC writes against studio-resolved channel indices.  The
 * channel-config side (reference, buffering) lives in devicetree on
 * the Zephyr backend; this header only exposes the runtime knobs apps
 * actually tune.
 *
 * Backends:
 *   - Zephyr   : `dac_*` driver class.  On the V2N family (V2N +
 *                V2N-M1, both of which carry the GD32G553 supervisor
 *                MCU) the SDK routes through the GD32 IO MCU bridge
 *                (per the 2026-05-12 hardware decision — see
 *                `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`).
 *   - Yocto    : Industrial I/O sysfs (`/sys/bus/iio/devices/`).
 *   - Baremetal: vendor HAL DAC channel registers.
 *
 * Typical usage:
 * @code
 *     alp_dac_t *out = alp_dac_open(&(alp_dac_config_t){
 *         .channel_id = E1M_DAC0,
 *         .initial_mv = 0u,
 *     });
 *     alp_dac_write_mv(out, 1650u);   // mid-rail on a 3.3 V reference
 *     alp_dac_close(out);
 * @endcode
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.2.  Base surface stable.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_DAC_H
#define ALP_DAC_H

#include <stdint.h>

#include "alp/cap_instance.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque DAC channel handle.  Allocate via @ref alp_dac_open. */
typedef struct alp_dac alp_dac_t;

/** Configuration passed to @ref alp_dac_open. */
typedef struct {
	uint32_t channel_id; /**< Studio-resolved DAC channel index (E1M_DAC0..DAC1). */
	uint16_t initial_mv; /**< Initial output in millivolts; 0 = ground. */
} alp_dac_config_t;

/**
 * @brief Acquire and initialise a DAC channel at @c initial_mv.
 *
 * Resolves @p cfg->channel_id to the underlying converter (a Zephyr
 * dac_* device on most SoMs; the GD32 IO MCU bridge on V2N), borrows
 * a free handle from the SDK's pool, and primes the output.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL; @c channel_id must
 *                 be < @ref E1M_DAC_COUNT and resolvable on the
 *                 active SoM.
 * @return Open handle on success, or NULL on any of:
 *         - @p cfg is NULL
 *         - @c channel_id out of range or unresolvable
 *         - underlying converter not ready
 *         - handle pool exhausted
 */
alp_dac_t *alp_dac_open(const alp_dac_config_t *cfg);

/**
 * @brief Set the DAC output in millivolts.
 *
 * The backend saturates at the DAC's reference rail (3.3 V typical
 * on the GD32 / V2N route) and rounds to the converter's hardware-
 * achievable resolution (12-bit on the GD32 DAC).
 *
 * @param[in] dac  Handle from @ref alp_dac_open.
 * @param[in] mv   Requested output in millivolts.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_IO.
 */
alp_status_t alp_dac_write_mv(alp_dac_t *dac, uint16_t mv);

/**
 * @brief Read back the currently-programmed DAC output in millivolts.
 *
 * Useful for verification + closed-loop monitors where the rounded
 * hardware setpoint may differ from the requested value.
 *
 * @param[in]  dac     Handle from @ref alp_dac_open.
 * @param[out] mv_out  Receives the programmed output (mV).
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t alp_dac_read_mv(alp_dac_t *dac, uint16_t *mv_out);

/**
 * @brief Release a DAC handle back to the pool.
 *
 * Does not power-down the converter or alter the output level — that
 * stays at the last-programmed value until the next open() reprograms
 * it.  NULL is a no-op.
 *
 * @param[in] dac  Handle from @ref alp_dac_open, or NULL.
 */
void alp_dac_close(alp_dac_t *dac);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_DAC_H */
