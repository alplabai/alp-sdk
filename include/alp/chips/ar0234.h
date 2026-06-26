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

#define AR0234_I2C_ADDR_LOW  0x10u /**< 7-bit SCCB address with SADDR strapped low. */
#define AR0234_I2C_ADDR_HIGH 0x18u /**< 7-bit SCCB address with SADDR strapped high. */

#define AR0234_REG_CHIP_VERSION 0x3000u /**< 16-bit chip-version register address. */
#define AR0234_CHIP_ID          0x0A56u /**< Expected value of CHIP_VERSION for the AR0234. */

/**
 * @brief AR0234 driver context (SCCB / I2C configuration side only).
 *
 * Covers the control channel; pixel data leaves over MIPI CSI-2 and is not
 * touched here.  @p bus is borrowed, not owned -- it must outlive the context.
 */
typedef struct {
	alp_i2c_t *bus;         /**< Caller-opened I2C/SCCB bus handle (borrowed). */
	uint8_t    addr;        /**< Active 7-bit slave address (AR0234_I2C_ADDR_*). */
	bool       initialised; /**< True between a successful init and deinit. */
} ar0234_t;

/**
 * @brief Bind the context to a bus and verify the chip ID.
 *
 * Reads AR0234_REG_CHIP_VERSION and checks it against AR0234_CHIP_ID; a mismatch
 * fails init so a wrong/absent part is caught early.  The bus is borrowed and
 * must stay valid until ar0234_deinit().
 *
 * @param dev       Context to initialise (output).
 * @param bus       Caller-opened I2C/SCCB bus handle.
 * @param i2c_addr  7-bit slave address (AR0234_I2C_ADDR_LOW or _HIGH).
 * @return          ALP_OK on success, ALP_ERR_NOT_READY if the ID read fails or
 *                  mismatches, ALP_ERR_INVAL on NULL args.
 */
alp_status_t ar0234_init(ar0234_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read the combined 16-bit chip-version register.
 *
 * @param dev     Initialised context.
 * @param id_out  Output: CHIP_VERSION value (AR0234_CHIP_ID for a genuine part).
 * @return        ALP_OK on success, ALP_ERR_NOT_READY if not initialised,
 *                ALP_ERR_INVAL on NULL args.
 */
alp_status_t ar0234_read_id(ar0234_t *dev, uint16_t *id_out);

/**
 * @brief Issue a software reset (RESET_REGISTER bit 0).
 *
 * Returns the sensor to its register defaults; allow the datasheet's reset
 * settling time before reconfiguring.
 *
 * @param dev  Initialised context.
 * @return     ALP_OK on success, ALP_ERR_NOT_READY if not initialised,
 *             ALP_ERR_INVAL on NULL @p dev.
 */
alp_status_t ar0234_soft_reset(ar0234_t *dev);

/**
 * @brief Release the driver context.  Idempotent; NULL tolerated.
 *
 * Does not close the borrowed bus handle -- the caller owns it.
 *
 * @param dev  Context to release (may be NULL).
 */
void ar0234_deinit(ar0234_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_AR0234_H */
