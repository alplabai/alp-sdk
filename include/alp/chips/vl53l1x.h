/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file vl53l1x.h
 * @brief STMicroelectronics VL53L1X time-of-flight ranger (I²C).
 *
 * 940 nm laser ToF sensor, up to 4 m range, ±2.5 % accuracy.
 * Driver covers chip-ID probe + soft reset only; the full ranging
 * sequence (sysrange-start, polling, range read) lands when the
 * ST ULD library import lands.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl]
 *
 * Datasheet: ST VL53L1X DS13088 Rev 7 (Jul 2021).
 */

#ifndef ALP_CHIPS_VL53L1X_H
#define ALP_CHIPS_VL53L1X_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Power-on 7-bit I²C address (re-addressable in software at runtime). */
#define VL53L1X_I2C_ADDR_DEFAULT 0x29u

/** @brief 16-bit register holding the device model identifier. */
#define VL53L1X_REG_IDENTIFICATION_MODEL_ID 0x010Fu
/** @brief Expected value at @ref VL53L1X_REG_IDENTIFICATION_MODEL_ID for a VL53L1X. */
#define VL53L1X_MODEL_ID 0xEAu

/**
 * @brief Driver context for one VL53L1X instance.
 *
 * Caller-allocated and owned; bound by @ref vl53l1x_init and torn down by
 * @ref vl53l1x_deinit.  Not thread-safe — serialise calls that share a
 * context (and the underlying I²C bus) externally.
 */
typedef struct {
	alp_i2c_t *bus;         /**< Caller-opened I²C bus; borrowed, not owned/closed here. */
	uint8_t    addr;        /**< 7-bit I²C address bound at init. */
	bool       initialised; /**< True once @ref vl53l1x_init verified the MODEL_ID. */
} vl53l1x_t;

/**
 * @brief Bind a context to an I²C bus and verify the MODEL_ID.
 *
 * @param[out] dev       Caller-allocated context to initialise.
 * @param[in]  bus       Opened I²C bus; borrowed for the context lifetime.
 * @param[in]  i2c_addr  7-bit address (typically @ref VL53L1X_I2C_ADDR_DEFAULT).
 * @return ALP_OK on success, or an ALP_ERR_* code on NULL args, I²C failure,
 *         or a MODEL_ID mismatch (wrong/absent device).
 */
alp_status_t vl53l1x_init(vl53l1x_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read the MODEL_ID register as a liveness/identity check.
 *
 * @param[in]  dev     Context bound by @ref vl53l1x_init.
 * @param[out] id_out  Receives the raw MODEL_ID byte (expect @ref VL53L1X_MODEL_ID).
 * @return ALP_OK on success, or an ALP_ERR_* code on NULL args or I²C failure.
 */
alp_status_t vl53l1x_read_id(vl53l1x_t *dev, uint8_t *id_out);

/**
 * @brief Issue a software reset via the SOFT_RESET register.
 *
 * @param[in] dev  Context bound by @ref vl53l1x_init.
 * @return ALP_OK on success, or an ALP_ERR_* code on NULL args or I²C failure.
 */
alp_status_t vl53l1x_soft_reset(vl53l1x_t *dev);

/**
 * @brief Release the driver context.  Does not close the borrowed I²C bus.
 *
 * @param[in] dev  Context to tear down; NULL is a no-op.
 */
void vl53l1x_deinit(vl53l1x_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_VL53L1X_H */
