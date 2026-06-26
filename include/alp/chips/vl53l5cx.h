/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file vl53l5cx.h
 * @brief STMicroelectronics VL53L5CX multi-zone time-of-flight ranger.
 *
 * 8 × 8 multi-zone ToF sensor (64 individually-resolved cells) up to
 * 4 m.  Same lifecycle shape as `vl53l1x` but with the additional
 * 64-cell distance / object-count read on top.  This driver covers
 * chip-ID probe + soft reset; the firmware boot + multi-zone read
 * path land alongside the ST ULD library import.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl]
 *
 * Datasheet: ST VL53L5CX DS13754 Rev 3 (Apr 2021).
 */

#ifndef ALP_CHIPS_VL53L5CX_H
#define ALP_CHIPS_VL53L5CX_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Power-on 7-bit I²C address (re-addressable in software at runtime). */
#define VL53L5CX_I2C_ADDR_DEFAULT 0x29u
/** @brief 16-bit register read to identify the device / confirm boot. */
#define VL53L5CX_REG_DEVICE_ID 0x0000u
#define VL53L5CX_DEVICE_ID     0xF0u /**< Expected boot-status low byte after fw load. */

/**
 * @brief Driver context for one VL53L5CX instance.
 *
 * Caller-allocated and owned; bound by @ref vl53l5cx_init and torn down by
 * @ref vl53l5cx_deinit.  Not thread-safe — serialise calls that share a
 * context (and the underlying I²C bus) externally.
 */
typedef struct {
	alp_i2c_t *bus;         /**< Caller-opened I²C bus; borrowed, not owned/closed here. */
	uint8_t    addr;        /**< 7-bit I²C address bound at init. */
	bool       initialised; /**< True once @ref vl53l5cx_init probed the device. */
} vl53l5cx_t;

/**
 * @brief Bind a context to an I²C bus and probe the device.
 *
 * @param[out] dev       Caller-allocated context to initialise.
 * @param[in]  bus       Opened I²C bus; borrowed for the context lifetime.
 * @param[in]  i2c_addr  7-bit address (typically @ref VL53L5CX_I2C_ADDR_DEFAULT).
 * @return ALP_OK on success, or an ALP_ERR_* code on NULL args, I²C failure,
 *         or a failed probe (wrong/absent device).
 */
alp_status_t vl53l5cx_init(vl53l5cx_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read the boot-status low byte as a liveness/identity check.
 *
 * @param[in]  dev     Context bound by @ref vl53l5cx_init.
 * @param[out] id_out  Receives the raw byte (expect @ref VL53L5CX_DEVICE_ID once booted).
 * @return ALP_OK on success, or an ALP_ERR_* code on NULL args or I²C failure.
 */
alp_status_t vl53l5cx_read_id(vl53l5cx_t *dev, uint8_t *id_out);

/**
 * @brief Issue a software reset.
 *
 * @param[in] dev  Context bound by @ref vl53l5cx_init.
 * @return ALP_OK on success, or an ALP_ERR_* code on NULL args or I²C failure.
 */
alp_status_t vl53l5cx_soft_reset(vl53l5cx_t *dev);

/**
 * @brief Release the driver context.  Does not close the borrowed I²C bus.
 *
 * @param[in] dev  Context to tear down; NULL is a no-op.
 */
void vl53l5cx_deinit(vl53l5cx_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_VL53L5CX_H */
