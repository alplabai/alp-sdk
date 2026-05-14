/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file veml7700.h
 * @brief Vishay VEML7700 high-precision ambient-light sensor (I²C).
 *
 * 16-bit lux-output ALS with on-chip gain + integration controls.
 * Less complex than TSL2591 (single visible-light channel) -- one
 * 16-bit count register maps directly to lux via a configurable
 * scale.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: Vishay VEML7700 Rev 1.5 (May 2020).
 */

#ifndef ALP_CHIPS_VEML7700_H
#define ALP_CHIPS_VEML7700_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VEML7700_I2C_ADDR 0x10u

#define VEML7700_REG_CONF      0x00u
#define VEML7700_REG_HIGH_TH   0x01u
#define VEML7700_REG_LOW_TH    0x02u
#define VEML7700_REG_POWER_SAVE 0x03u
#define VEML7700_REG_ALS       0x04u
#define VEML7700_REG_WHITE     0x05u

typedef struct {
    alp_i2c_t *bus;
    uint8_t    addr;
    bool       initialised;
} veml7700_t;

/** @brief Bind context and turn on the ALS (clears CONF bit 0). */
alp_status_t veml7700_init(veml7700_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Read raw 16-bit ALS counts. */
alp_status_t veml7700_read_als(veml7700_t *dev, uint16_t *als_out);

/** @brief Release driver context. */
void veml7700_deinit(veml7700_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_VEML7700_H */
