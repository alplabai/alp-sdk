/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file max31865.h
 * @brief ADI (Maxim) MAX31865 RTD (PT100 / PT1000) digitiser (SPI).
 *
 * 15-bit RTD-to-digital converter with on-chip Wheatstone bridge
 * + fault detection.  Same family as MAX31855 (thermocouple);
 * different driver because RTD bridge configuration is more
 * involved.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [complete-impl] — config-write + RTD-read with
 *   fault-flag reporting.
 *
 * Datasheet: ADI (Maxim) MAX31865 (Rev 3, Aug 2015).
 */

#ifndef ALP_CHIPS_MAX31865_H
#define ALP_CHIPS_MAX31865_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX31865_REG_CONFIG     0x00u /* RW (write 0x80 | reg). */
#define MAX31865_REG_RTD_MSB    0x01u
#define MAX31865_REG_RTD_LSB    0x02u
#define MAX31865_REG_FAULT_STAT 0x07u

#define MAX31865_CONFIG_VBIAS        0x80u
#define MAX31865_CONFIG_MODE_AUTO    0x40u
#define MAX31865_CONFIG_3WIRE        0x10u
#define MAX31865_CONFIG_FAULT_AUTO   0x04u
#define MAX31865_CONFIG_CLEAR_FAULTS 0x02u
#define MAX31865_CONFIG_FILTER_50HZ  0x01u

typedef struct {
	alp_spi_t *bus;
	bool       initialised;
} max31865_t;

/** @brief Bind context to caller-opened SPI bus. */
alp_status_t max31865_init(max31865_t *dev, alp_spi_t *spi);

/** @brief Write CONFIG register. */
alp_status_t max31865_set_config(max31865_t *dev, uint8_t config);

/**
 * @brief Read 15-bit RTD ratio (0..32767).
 *
 * Note: fault flag is in bit 0 of RTD_LSB; cleared from returned
 * value but reflected in *fault_set if non-NULL.
 */
alp_status_t max31865_read_rtd(max31865_t *dev, uint16_t *rtd_out, bool *fault_set);

/** @brief Release driver context. */
void max31865_deinit(max31865_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_MAX31865_H */
