/*
 * Copyright 2026 ALP Lab AB
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

/** Fault bits in the 32-bit reading. */
#define MAX31855_FAULT_MASK 0x00010007u

typedef struct {
    alp_spi_t *bus;
    bool       initialised;
} max31855_t;

/** @brief Bind context to caller-opened SPI bus. */
alp_status_t max31855_init(max31855_t *dev, alp_spi_t *spi);

/**
 * @brief Read the raw 32-bit reading + decode temperatures.
 *
 * @param dev               Initialised context.
 * @param tc_milli_c        Output: thermocouple temperature (milli-C).  Optional.
 * @param internal_milli_c  Output: internal cold-junction (milli-C). Optional.
 * @param fault_flags       Output: bitmap of fault flags.            Optional.
 * @return `ALP_OK` on success.
 */
alp_status_t max31855_read(max31855_t *dev,
                           int32_t    *tc_milli_c,
                           int32_t    *internal_milli_c,
                           uint8_t    *fault_flags);

/** @brief Release driver context. */
void max31855_deinit(max31855_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_MAX31855_H */
