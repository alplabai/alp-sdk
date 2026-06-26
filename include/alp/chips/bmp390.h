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

#define BMP390_REG_CHIP_ID 0x00u /**< CHIP_ID register address. */
#define BMP390_CHIP_ID     0x60u /**< Expected CHIP_ID value (shared with BMP388). */

/** @brief Driver context.  Treat as opaque; populated by @ref bmp390_init. */
typedef struct {
	alp_i2c_t *bus;         /**< Caller-owned I2C bus the chip lives on. */
	uint8_t    addr;        /**< 7-bit I2C address (see BMP390_I2C_ADDR_*). */
	bool       initialised; /**< True once @ref bmp390_init has verified CHIP_ID. */
} bmp390_t;

/**
 * @brief Bind a driver context to an open I2C bus and verify CHIP_ID.
 *
 * Reads CHIP_ID and checks it equals @ref BMP390_CHIP_ID.  Ownership of
 * @p bus stays with the caller; @ref bmp390_deinit does not close it.  Does
 * not start sampling.
 *
 * @param dev      Context to initialise.  Must be non-NULL.
 * @param bus      Open I2C bus.  Must be non-NULL.
 * @param i2c_addr 7-bit address (see BMP390_I2C_ADDR_*).  Must be non-zero.
 * @return ALP_OK on success; ALP_ERR_INVAL on a NULL/zero argument;
 *         ALP_ERR_IO on a CHIP_ID mismatch (chip absent or not a BMP390);
 *         otherwise the propagated I2C status.
 */
alp_status_t bmp390_init(bmp390_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read the CHIP_ID register for liveness checks post-init.
 *
 * @param dev    Initialised context.  Must be non-NULL.
 * @param id_out Receives the CHIP_ID byte.  Must be non-NULL.
 * @return ALP_OK on success; ALP_ERR_NOT_READY if @p dev is uninitialised;
 *         ALP_ERR_INVAL if @p id_out is NULL; otherwise the I2C status.
 */
alp_status_t bmp390_read_id(bmp390_t *dev, uint8_t *id_out);

/**
 * @brief Issue a soft reset (writes 0xB6 to the CMD register).
 *
 * Returns after the write; the caller must wait at least 2 ms before the next
 * register access per datasheet.  Re-run @ref bmp390_init afterwards.
 *
 * @param dev Initialised context.  Must be non-NULL.
 * @return ALP_OK on success; ALP_ERR_NOT_READY if @p dev is uninitialised;
 *         otherwise the propagated I2C status.
 */
alp_status_t bmp390_soft_reset(bmp390_t *dev);

/**
 * @brief Release the driver context.
 *
 * Clears @p dev's state; does not close the I2C bus or power down the chip.
 * NULL @p dev is a no-op.
 *
 * @param dev Context to release.  May be NULL.
 */
void bmp390_deinit(bmp390_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_BMP390_H */
