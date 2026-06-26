/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file gc2145.h
 * @brief GalaxyCore GC2145 2 MP DVP camera — config-side driver.
 *
 * China-domestic cost-sensitive 2 MP camera popular in Arduino-
 * Portenta H7's Vision Shield and budget eval boards.  DVP 8-bit
 * parallel pixel egress; SCCB config side only.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl]
 */

#ifndef ALP_CHIPS_GC2145_H
#define ALP_CHIPS_GC2145_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GC2145_I2C_ADDR 0x3Cu /**< Default 7-bit SCCB/I2C address. */

#define GC2145_REG_PID_HI 0xF0u   /**< Product-ID high byte register. */
#define GC2145_REG_PID_LO 0xF1u   /**< Product-ID low byte register. */
#define GC2145_CHIP_ID    0x2145u /**< Expected (PID_HI<<8 | PID_LO) value. */

/** @brief GC2145 camera config-side context. Pixel data egresses over the DVP bus. */
typedef struct {
	alp_i2c_t *bus;         /**< Borrowed SCCB/I2C bus (caller-owned, kept open). */
	uint8_t    addr;        /**< 7-bit I2C address (default GC2145_I2C_ADDR). */
	bool       initialised; /**< True once gc2145_init() verified the chip ID. */
} gc2145_t;

/**
 * @brief Bind a context to a caller-opened SCCB/I2C bus and verify the chip ID.
 *
 * Reads the product-ID registers and checks them against @ref GC2145_CHIP_ID.
 * The @p bus handle is borrowed, not owned — keep it open for the context
 * lifetime.
 *
 * @param dev       Output: caller-allocated context to populate.
 * @param bus       Open SCCB/I2C bus the camera's config port sits on.
 * @param i2c_addr  7-bit I2C address (default GC2145_I2C_ADDR).
 * @return `ALP_OK` if the chip ID matches; an `ALP_ERR_*` status otherwise.
 */
alp_status_t gc2145_init(gc2145_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read the product/chip ID for liveness checking.
 *
 * @param dev     Initialised context.
 * @param id_out  Output: receives (PID_HI<<8 | PID_LO); compare to GC2145_CHIP_ID.
 * @return `ALP_OK` on success; an `ALP_ERR_*` status on a bus error.
 */
alp_status_t gc2145_read_id(gc2145_t *dev, uint16_t *id_out);

/**
 * @brief Issue a software reset (sets bit 7 of register 0xFE).
 *
 * @param dev  Initialised context.
 * @return `ALP_OK` on success; an `ALP_ERR_*` status on a bus error.
 */
alp_status_t gc2145_soft_reset(gc2145_t *dev);

/**
 * @brief Release the driver context. NULL @p dev is tolerated.
 *
 * Clears the bound bus reference but does not close it — the caller owns the
 * I2C handle.
 *
 * @param dev  Context to release (may be NULL).
 */
void gc2145_deinit(gc2145_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_GC2145_H */
