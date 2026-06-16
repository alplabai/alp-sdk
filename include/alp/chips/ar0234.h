/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ar0234.h
 * @brief onsemi AR0234CS 1080p global-shutter colour MIPI CSI-2 sensor.
 *
 * 1/2.6-inch industrial 1080p60 global-shutter sensor with HDR.
 * Common in machine-vision boards.  SCCB config side only.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl]
 *
 * Datasheet: onsemi AR0234CS (Rev 7, Jun 2020).
 */

#ifndef ALP_CHIPS_AR0234_H
#define ALP_CHIPS_AR0234_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AR0234_I2C_ADDR_LOW 0x10u
#define AR0234_I2C_ADDR_HIGH 0x18u

#define AR0234_REG_CHIP_VERSION 0x3000u
#define AR0234_CHIP_ID 0x0A56u

typedef struct {
	alp_i2c_t *bus;
	uint8_t    addr;
	bool       initialised;
} ar0234_t;

/** @brief Bind context and verify chip ID. */
alp_status_t ar0234_init(ar0234_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Read chip-version (combined 16-bit). */
alp_status_t ar0234_read_id(ar0234_t *dev, uint16_t *id_out);

/** @brief Issue a software reset (RESET_REGISTER bit 0). */
alp_status_t ar0234_soft_reset(ar0234_t *dev);

/** @brief Release driver context.  Idempotent.  NULL tolerated. */
void ar0234_deinit(ar0234_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_AR0234_H */
