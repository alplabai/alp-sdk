/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bmp390.h
 * @brief Bosch BMP390 high-precision barometric pressure sensor (I²C).
 *
 * 24-bit pressure + temperature sensor with ±0.03 hPa absolute
 * accuracy.  Same family as BMP581 already shipped under
 * `chips/bmp581/`; BMP390 supersedes the older BMP388.  4-wire
 * SPI is also supported by silicon but this driver covers I²C
 * only — SPI variant lives in a follow-up.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: Bosch BMP390 v1.4 (Nov 2020).
 */

#ifndef ALP_CHIPS_BMP390_H
#define ALP_CHIPS_BMP390_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BMP390_I2C_ADDR_PRIMARY   0x77u /**< SDO = high (default). */
#define BMP390_I2C_ADDR_SECONDARY 0x76u /**< SDO = low. */

#define BMP390_REG_CHIP_ID 0x00u
#define BMP390_CHIP_ID     0x60u

typedef struct {
	alp_i2c_t *bus;
	uint8_t    addr;
	bool       initialised;
} bmp390_t;

/** @brief Bind context and verify CHIP_ID == 0x60. */
alp_status_t bmp390_init(bmp390_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Read CHIP_ID register. */
alp_status_t bmp390_read_id(bmp390_t *dev, uint8_t *id_out);

/**
 * @brief Soft reset (CMD register, 0xB6).
 *
 * Returns after writing; caller must wait at least 2 ms before
 * the next register access per datasheet.
 */
alp_status_t bmp390_soft_reset(bmp390_t *dev);

/** @brief Release driver context. */
void bmp390_deinit(bmp390_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_BMP390_H */
