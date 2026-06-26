/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file imx477.h
 * @brief Sony IMX477 12.3 MP MIPI CSI-2 sensor (Raspberry Pi HQ Camera).
 *
 * 1/2.3-inch 4056 × 3040 Bayer sensor used in the RPi HQ Camera
 * module.  SCCB config side only; pixel data flows over MIPI CSI-2.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl]
 */

#ifndef ALP_CHIPS_IMX477_H
#define ALP_CHIPS_IMX477_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMX477_I2C_ADDR 0x1Au /**< Default 7-bit SCCB/I2C address. */

#define IMX477_REG_MODEL_ID_HI 0x0016u /**< MODEL_ID high byte register. */
#define IMX477_REG_MODEL_ID_LO 0x0017u /**< MODEL_ID low byte register. */
#define IMX477_CHIP_ID         0x0477u /**< Expected 16-bit MODEL_ID value. */

/** @brief Driver context for one IMX477 sensor.  Treat as opaque. */
typedef struct {
	alp_i2c_t *bus;         /**< SCCB/I2C config bus (borrowed; must outlive the context). */
	uint8_t    addr;        /**< 7-bit I2C address in use. */
	bool       initialised; /**< True once imx477_init() validated the chip ID. */
} imx477_t;

/**
 * @brief Bind a context to an SCCB/I2C bus and verify the sensor chip ID.
 *
 * Zeroes @p dev, stores the bus + address, then reads MODEL_ID and checks it
 * against @ref IMX477_CHIP_ID.  Configures the SCCB side only; pixel data
 * flows over MIPI CSI-2 and is out of scope here.
 *
 * @param[out] dev       Context to populate.
 * @param[in]  bus       Open SCCB/I2C bus the sensor sits on (borrowed, not owned).
 * @param[in]  i2c_addr  7-bit I2C address; must be non-zero (e.g. @ref IMX477_I2C_ADDR).
 * @return ALP_OK on success; ALP_ERR_INVAL if @p dev / @p bus is NULL or
 *         @p i2c_addr is 0; ALP_ERR_IO on chip-ID mismatch; or the underlying
 *         I2C error status on bus failure.
 */
alp_status_t imx477_init(imx477_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read the 16-bit MODEL_ID register for liveness checks.
 *
 * @param[in]  dev     Initialised context.
 * @param[out] id_out  Receives the MODEL_ID (expected @ref IMX477_CHIP_ID).
 * @return ALP_OK on success; ALP_ERR_NOT_READY if @p dev is NULL or
 *         uninitialised; ALP_ERR_INVAL if @p id_out is NULL; or the I2C
 *         error status on bus failure.
 */
alp_status_t imx477_read_id(imx477_t *dev, uint16_t *id_out);

/**
 * @brief Issue a software reset (register 0x0103, bit 0).
 *
 * @param[in] dev  Initialised context.
 * @return ALP_OK on success; ALP_ERR_NOT_READY if @p dev is NULL or
 *         uninitialised; or the I2C error status on bus failure.
 */
alp_status_t imx477_soft_reset(imx477_t *dev);

/**
 * @brief Release the driver context.  NULL tolerated; does not power down the chip.
 *
 * Clears the initialised flag and drops the bus reference; the underlying bus
 * is not closed (the caller still owns it).
 *
 * @param[in,out] dev  Context to release (may be NULL).
 */
void imx477_deinit(imx477_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_IMX477_H */
