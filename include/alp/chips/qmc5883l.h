/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file qmc5883l.h
 * @brief QST QMC5883L 3-axis magnetic compass (I²C).
 *
 * Drop-in replacement for the HMC5883L (discontinued).  Three
 * 16-bit ADCs producing signed counts; the integrating
 * application converts to micro-Tesla using the configured range
 * (default 2 G full-scale).
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: QST QMC5883L v1.0 (Sep 2016).
 */

#ifndef ALP_CHIPS_QMC5883L_H
#define ALP_CHIPS_QMC5883L_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QMC5883L_I2C_ADDR 0x0Du

#define QMC5883L_REG_DATA_X_LO 0x00u
#define QMC5883L_REG_CTRL1     0x09u
#define QMC5883L_REG_CTRL2     0x0Au
#define QMC5883L_REG_SET_RESET 0x0Bu
#define QMC5883L_REG_CHIP_ID   0x0Du

#define QMC5883L_CHIP_ID 0xFFu /**< QMC datasheet says 0xFF; clones may report 0xC4. */

typedef struct {
	int16_t x;
	int16_t y;
	int16_t z;
} qmc5883l_axes_t;

typedef struct {
	alp_i2c_t *bus;
	uint8_t    addr;
	bool       initialised;
} qmc5883l_t;

/** @brief Bind context and apply continuous-mode CTRL defaults. */
alp_status_t qmc5883l_init(qmc5883l_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Read 3-axis magnetic field as signed 16-bit counts. */
alp_status_t qmc5883l_read_axes(qmc5883l_t *dev, qmc5883l_axes_t *axes_out);

/** @brief Release driver context. */
void qmc5883l_deinit(qmc5883l_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_QMC5883L_H */
