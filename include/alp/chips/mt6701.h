/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file mt6701.h
 * @brief MagnTek MT6701 14-bit magnetic rotary encoder (I²C).
 *
 * Cost-competitive alternative to AS5048B.  Same shape: read two
 * registers for the 14-bit absolute angle.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: MagnTek MT6701 v2.0 (Nov 2021).
 */

#ifndef ALP_CHIPS_MT6701_H
#define ALP_CHIPS_MT6701_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MT6701_I2C_ADDR 0x06u /**< Fixed 7-bit I²C address. */

#define MT6701_REG_ANGLE_HI 0x03u /**< Angle bits [13:6] (high byte). */
#define MT6701_REG_ANGLE_LO 0x04u /**< Angle bits [5:0] in this register's bits [7:2]. */

/** @brief MT6701 magnetic rotary-encoder driver context. */
typedef struct {
	alp_i2c_t *bus;         /**< Open I²C bus (not owned). */
	uint8_t    addr;        /**< 7-bit device address. */
	bool       initialised; /**< True once mt6701_init() succeeded. */
} mt6701_t;

/**
 * @brief Bind context to an open I²C bus.
 *
 * @param dev       Driver context to populate (output).
 * @param bus       Open I²C bus (not owned; must outlive @p dev).
 * @param i2c_addr  7-bit address (typically MT6701_I2C_ADDR).
 * @return ALP_OK on success; an ALP_ERR_* status on failure.
 */
alp_status_t mt6701_init(mt6701_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read 14-bit absolute angle (0..16383).
 *
 * @param dev        Initialised context.
 * @param angle_out  Output: angle in counts (0..16383, 360°/16384 per count).
 * @return ALP_OK on success; an ALP_ERR_* status on I²C failure.
 */
alp_status_t mt6701_read_angle(mt6701_t *dev, uint16_t *angle_out);

/**
 * @brief Release driver context.
 *
 * @param dev  Driver context, or NULL (tolerated; no-op).
 */
void mt6701_deinit(mt6701_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_MT6701_H */
