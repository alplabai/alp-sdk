/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file max31855.h
 * @brief ADI (Maxim) MAX31855 K-type thermocouple-to-digital converter (SPI).
 *
 * Read-only 14-bit thermocouple ADC with cold-junction compensation.
 * Reports a single 32-bit packed word per read containing
 * thermocouple temperature + internal reference temperature +
 * fault flags (OC / SCG / SCV).
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: ADI (Maxim) MAX31855 (Rev 4, Aug 2015).
 */

#ifndef ALP_CHIPS_MAX31855_H
#define ALP_CHIPS_MAX31855_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fault bits within the raw 32-bit reading.
 *
 * Covers the fault summary (bit 16) plus the three low-byte cause bits:
 * OC (open circuit, bit 0), SCG (short to GND, bit 1), SCV (short to VCC, bit 2).
 */
#define MAX31855_FAULT_MASK 0x00010007u

/**
 * @brief Driver context for one MAX31855. Treat as opaque; populated by
 *        @ref max31855_init.
 */
typedef struct {
	alp_spi_t *bus;         /**< Borrowed SPI bus; owned by the caller, not freed here. */
	bool       initialised; /**< True once @ref max31855_init has run. */
} max31855_t;

/**
 * @brief Bind a context to a caller-opened SPI bus.
 *
 * The MAX31855 is read-only and has no registers to configure, so this only
 * records the bus handle. Bus mode/speed must be set up by the caller
 * (SPI mode 0, up to 5 MHz).
 *
 * @param dev Context to initialise. Must be non-NULL.
 * @param spi Open SPI bus; borrowed, must outlive @p dev.
 * @return ALP_OK on success, or an error if arguments are invalid.
 */
alp_status_t max31855_init(max31855_t *dev, alp_spi_t *spi);

/**
 * @brief Read the raw 32-bit reading + decode temperatures.
 *
 * @param dev               Initialised context.
 * @param tc_milli_c        Output: thermocouple temperature (milli-C).  Optional.
 * @param internal_milli_c  Output: internal cold-junction (milli-C). Optional.
 * @param fault_flags       Output: bitmap of fault flags (OC/SCG/SCV). Optional.
 * @return `ALP_OK` on success, or an error from the SPI transfer. When any
 *         fault bit is set the temperature outputs are not meaningful; inspect
 *         @p fault_flags to distinguish the cause.
 */
alp_status_t max31855_read(max31855_t *dev,
                           int32_t    *tc_milli_c,
                           int32_t    *internal_milli_c,
                           uint8_t    *fault_flags);

/**
 * @brief Release the driver context. Does not close the borrowed SPI bus.
 *
 * @param dev Context to release; NULL is ignored.
 */
void max31855_deinit(max31855_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_MAX31855_H */
