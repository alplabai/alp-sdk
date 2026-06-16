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

#define MT6701_I2C_ADDR 0x06u

#define MT6701_REG_ANGLE_HI 0x03u
#define MT6701_REG_ANGLE_LO 0x04u

typedef struct {
	alp_i2c_t *bus;
	uint8_t    addr;
	bool       initialised;
} mt6701_t;

/** @brief Bind context to an open I²C bus. */
alp_status_t mt6701_init(mt6701_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read 14-bit absolute angle (0..16383).
 *
 * @param dev        Initialised context.
 * @param angle_out  Output: angle in counts.
 */
alp_status_t mt6701_read_angle(mt6701_t *dev, uint16_t *angle_out);

/** @brief Release driver context. */
void mt6701_deinit(mt6701_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_MT6701_H */
