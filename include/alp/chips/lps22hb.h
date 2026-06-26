/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lps22hb.h
 * @brief STMicroelectronics LPS22HB pressure sensor (I²C).
 *
 * 24-bit pressure + temperature MEMS barometer, ±0.1 hPa absolute
 * accuracy.  Common in ST's IoT eval boards alongside the
 * `lis2dw12` accelerometer already shipped.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: ST LPS22HB Rev 6 (Mar 2018).
 */

#ifndef ALP_CHIPS_LPS22HB_H
#define ALP_CHIPS_LPS22HB_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LPS22HB_I2C_ADDR_LOW  0x5Cu /**< SA0 = low. */
#define LPS22HB_I2C_ADDR_HIGH 0x5Du /**< SA0 = high (default). */

#define LPS22HB_REG_WHO_AM_I 0x0Fu /**< Device-identification register address. */
#define LPS22HB_WHO_AM_I     0xB1u /**< Fixed value WHO_AM_I returns for an LPS22HB. */

/**
 * @brief Driver context for one LPS22HB. Treat as opaque; populated by
 *        @ref lps22hb_init and consumed by the other entry points.
 */
typedef struct {
	alp_i2c_t *bus;         /**< Borrowed I2C bus handle; owned by the caller, not freed here. */
	uint8_t    addr;        /**< 7-bit I2C address in use (see LPS22HB_I2C_ADDR_*). */
	bool       initialised; /**< True once @ref lps22hb_init has validated WHO_AM_I. */
} lps22hb_t;

/**
 * @brief Bind a context to an open I2C bus and verify WHO_AM_I = 0xB1.
 *
 * Does not write any configuration registers; the device is left in its
 * power-on default state. Safe to call again to re-bind after deinit.
 *
 * @param dev      Context to initialise. Must be non-NULL.
 * @param bus      Open I2C bus; borrowed, must outlive @p dev.
 * @param i2c_addr 7-bit address (LPS22HB_I2C_ADDR_LOW or _HIGH).
 * @return ALP_OK on success; ALP_ERR_IO if the WHO_AM_I check fails
 *         (chip absent or not an LPS22HB), or an error from the I2C layer.
 */
alp_status_t lps22hb_init(lps22hb_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read the WHO_AM_I register (liveness check).
 *
 * @param dev    Initialised context.
 * @param id_out Receives the register value (expected 0xB1). Must be non-NULL.
 * @return ALP_OK on success, or an error from the I2C transfer.
 */
alp_status_t lps22hb_read_id(lps22hb_t *dev, uint8_t *id_out);

/**
 * @brief Issue a software reset (CTRL_REG2 SWRESET bit).
 *
 * Restores power-on register defaults. The caller is responsible for any
 * settle delay before re-configuring the device.
 *
 * @param dev Initialised context.
 * @return ALP_OK on success, or an error from the I2C transfer.
 */
alp_status_t lps22hb_soft_reset(lps22hb_t *dev);

/**
 * @brief Release the driver context. Does not power down the chip and does
 *        not close the borrowed I2C bus.
 *
 * @param dev Context to release; NULL is ignored.
 */
void lps22hb_deinit(lps22hb_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_LPS22HB_H */
