/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file as5048a_b.h
 * @brief ams AS5048A / AS5048B 14-bit magnetic rotary encoder.
 *
 * AS5048A speaks SPI; AS5048B speaks I²C.  This driver covers the
 * I²C variant — AS5048B at 7-bit address 0x40 (strap-selectable
 * 0x40..0x43).  SPI variant lives in a parallel `as5048a_spi`
 * driver in a follow-up.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: ams AS5048A/B v1.7 (Nov 2018).
 */

#ifndef ALP_CHIPS_AS5048A_B_H
#define ALP_CHIPS_AS5048A_B_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AS5048B_I2C_ADDR_BASE 0x40u /**< Base 7-bit address; OR with 0..3 (A1:A2 strap). */

#define AS5048B_REG_ANGLE_HI 0xFEu /**< Angle register, upper 8 bits (bits 13:6). */
#define AS5048B_REG_ANGLE_LO 0xFFu /**< Angle register, lower 6 bits (bits 5:0). */

/**
 * @brief AS5048B driver context (I2C variant).
 *
 * @p bus is borrowed, not owned -- it must outlive the context.
 */
typedef struct {
	alp_i2c_t *bus;         /**< Caller-opened I2C bus handle (borrowed). */
	uint8_t    addr;        /**< Active 7-bit slave address (0x40..0x43). */
	bool       initialised; /**< True between a successful init and deinit. */
} as5048b_t;

/**
 * @brief Bind the context to a caller-opened I2C bus.
 *
 * The bus is borrowed and must stay valid until as5048b_deinit().
 *
 * @param dev       Context to initialise (output).
 * @param bus       Caller-opened I2C bus handle.
 * @param i2c_addr  7-bit slave address (AS5048B_I2C_ADDR_BASE | strap, 0x40..0x43).
 * @return          ALP_OK on success, ALP_ERR_INVAL on NULL args.
 */
alp_status_t as5048b_init(as5048b_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read the 14-bit absolute angle in raw counts.
 *
 * One full mechanical revolution spans the whole range, so degrees =
 * counts * 360 / 16384.  The value is the raw angle; zero-position and rotation
 * direction depend on magnet placement and any programmed offset.
 *
 * @param dev        Initialised context.
 * @param angle_out  Output: angle in counts, 0..16383.
 * @return           ALP_OK on success, ALP_ERR_NOT_READY if not initialised,
 *                   ALP_ERR_INVAL on NULL args.
 */
alp_status_t as5048b_read_angle(as5048b_t *dev, uint16_t *angle_out);

/**
 * @brief Release the driver context.  Idempotent; NULL tolerated.
 *
 * Does not close the borrowed bus handle -- the caller owns it.
 *
 * @param dev  Context to release (may be NULL).
 */
void as5048b_deinit(as5048b_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_AS5048A_B_H */
