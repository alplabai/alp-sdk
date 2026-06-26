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

/** @brief One magnetometer sample, signed ADC counts (scale per configured range). */
typedef struct {
	int16_t x; /**< X-axis field, signed counts. */
	int16_t y; /**< Y-axis field, signed counts. */
	int16_t z; /**< Z-axis field, signed counts. */
} qmc5883l_axes_t;

/** @brief Driver context for one QMC5883L on an I2C bus. */
typedef struct {
	alp_i2c_t *bus;         /**< Caller-opened I2C bus handle (borrowed, not owned). */
	uint8_t    addr;        /**< 7-bit I2C address (::QMC5883L_I2C_ADDR). */
	bool       initialised; /**< True once qmc5883l_init() has succeeded. */
} qmc5883l_t;

/**
 * @brief Bind context and apply continuous-mode CTRL defaults.
 *
 * @param dev      Caller-allocated context to populate.
 * @param bus      Open I2C bus handle (borrowed; must outlive @p dev).
 * @param i2c_addr 7-bit device address (use ::QMC5883L_I2C_ADDR).
 * @return ALP_OK on success; ALP_ERR_INVAL on NULL argument; an I2C
 *         error status if the CTRL register writes fail.
 */
alp_status_t qmc5883l_init(qmc5883l_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read 3-axis magnetic field as signed 16-bit counts.
 *
 * Convert to micro-Tesla in the application using the configured
 * full-scale range (default 2 G).
 *
 * @param dev      Initialised driver context.
 * @param axes_out Out-param; receives the X/Y/Z sample.
 * @return ALP_OK on success; ALP_ERR_INVAL on NULL argument; an I2C
 *         error status on a failed read.
 */
alp_status_t qmc5883l_read_axes(qmc5883l_t *dev, qmc5883l_axes_t *axes_out);

/**
 * @brief Release driver context.
 *
 * @param dev Driver context; NULL is tolerated as a no-op.
 */
void qmc5883l_deinit(qmc5883l_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_QMC5883L_H */
