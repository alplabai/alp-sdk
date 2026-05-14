/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ms5611.h
 * @brief TE Connectivity MS5611 high-altitude barometer (I²C).
 *
 * Drone-grade barometer with built-in 24-bit ΔΣ ADC and on-chip
 * factory calibration PROM (six 16-bit coefficients).  Used in
 * essentially every consumer + commercial quadcopter sold.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *
 * Datasheet: TE Connectivity MS5611-01BA03 v5.0 (Oct 2014).
 */

#ifndef ALP_CHIPS_MS5611_H
#define ALP_CHIPS_MS5611_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MS5611_I2C_ADDR_PRIMARY   0x77u /**< CSB low / VDD high (default). */
#define MS5611_I2C_ADDR_SECONDARY 0x76u

#define MS5611_CMD_RESET     0x1Eu
#define MS5611_CMD_PROM_BASE 0xA0u /**< OR with 2*i for coefficient i in [0..7]. */

typedef struct {
    alp_i2c_t *bus;
    uint8_t    addr;
    uint16_t   prom[8]; /**< Factory calibration (read at init). */
    bool       initialised;
} ms5611_t;

/**
 * @brief Bind context, soft-reset, read PROM coefficients.
 *
 * @return `ALP_OK` on success.
 */
alp_status_t ms5611_init(ms5611_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Reissue the CMD_RESET soft reset (~2.8 ms quiet time after). */
alp_status_t ms5611_soft_reset(ms5611_t *dev);

/** @brief Return cached PROM coefficient `idx` in [0..7]. */
alp_status_t ms5611_get_coefficient(ms5611_t *dev, uint8_t idx, uint16_t *coef_out);

/** @brief Release driver context. */
void ms5611_deinit(ms5611_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_MS5611_H */
