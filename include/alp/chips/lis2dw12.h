/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lis2dw12.h
 * @brief STMicroelectronics LIS2DW12 3-axis ultra-low-power accelerometer.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Public surface consumed by alp-studio block `blk_accel_lis2dw12`.
 * Symbols carry the chip's natural prefix `lis2dw12_*` — no `alp_`.
 *
 * v0.2 scope: I2C only, ODR + full-scale + power-mode configuration,
 * raw 14-bit accel reads.  SPI lands in v0.3 alongside FIFO support.
 *
 * Datasheet: STMicroelectronics LIS2DW12 (DocID030334, rev 4, Mar 2018).
 */

#ifndef ALP_CHIPS_LIS2DW12_H
#define ALP_CHIPS_LIS2DW12_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default 7-bit I2C address with SDO/SA0 tied low. */
#define LIS2DW12_I2C_ADDR_LOW 0x18
/** Default 7-bit I2C address with SDO/SA0 tied high. */
#define LIS2DW12_I2C_ADDR_HIGH 0x19

/** WHO_AM_I register value the chip returns. */
#define LIS2DW12_WHO_AM_I_VAL 0x44

/** @brief Output data rate (CTRL1 bits[7:4]).  Actual frequency depends on power mode. */
typedef enum {
	LIS2DW12_ODR_OFF     = 0x0, /**< Power-down; no sampling. */
	LIS2DW12_ODR_1_6_HZ  = 0x1, /**< Low-power mode only; 12.5 Hz in HP. */
	LIS2DW12_ODR_12_5_HZ = 0x2, /**< 12.5 Hz. */
	LIS2DW12_ODR_25_HZ   = 0x3, /**< 25 Hz. */
	LIS2DW12_ODR_50_HZ   = 0x4, /**< 50 Hz. */
	LIS2DW12_ODR_100_HZ  = 0x5, /**< 100 Hz. */
	LIS2DW12_ODR_200_HZ  = 0x6, /**< 200 Hz. */
	LIS2DW12_ODR_400_HZ  = 0x7, /**< High-performance mode only. */
	LIS2DW12_ODR_800_HZ  = 0x8, /**< High-performance mode only. */
	LIS2DW12_ODR_1600_HZ = 0x9  /**< High-performance mode only. */
} lis2dw12_odr_t;

/** @brief Acceleration full-scale range (CTRL6 bits[5:4]). */
typedef enum {
	LIS2DW12_FS_2G  = 0x0, /**< +/-2 g. */
	LIS2DW12_FS_4G  = 0x1, /**< +/-4 g. */
	LIS2DW12_FS_8G  = 0x2, /**< +/-8 g. */
	LIS2DW12_FS_16G = 0x3  /**< +/-16 g. */
} lis2dw12_fs_t;

/** @brief Power / resolution mode (CTRL1 bits[3:2] paired with LP_MODE bits[1:0]). */
typedef enum {
	LIS2DW12_MODE_LOW_POWER_12BIT = 0x0, /**< 12-bit, lowest current. */
	LIS2DW12_MODE_LOW_POWER_14BIT = 0x1, /**< 14-bit low-power. */
	LIS2DW12_MODE_HIGH_PERF_14BIT = 0x2, /**< Full bandwidth. */
	LIS2DW12_MODE_SINGLE_SHOT     = 0x3  /**< One sample on demand. */
} lis2dw12_mode_t;

/** @brief Three-axis sample, raw 16-bit signed counts (left-justified). */
typedef struct {
	int16_t x; /**< X-axis raw count. */
	int16_t y; /**< Y-axis raw count. */
	int16_t z; /**< Z-axis raw count. */
} lis2dw12_axes_t;

/** @brief Driver context.  Treat as opaque. */
typedef struct {
	alp_i2c_t      *bus;         /**< I2C bus the chip sits on (borrowed; must outlive ctx). */
	uint8_t         addr;        /**< 7-bit I2C address in use. */
	lis2dw12_fs_t   fs;          /**< Full-scale range last set via lis2dw12_set_accel(). */
	lis2dw12_mode_t mode;        /**< Power/resolution mode last set via lis2dw12_set_accel(). */
	bool            initialised; /**< True once lis2dw12_init() validated WHO_AM_I. */
} lis2dw12_t;

/**
 * @brief Bind a driver context to an open I2C bus.
 *
 * Reads WHO_AM_I and verifies it matches @ref LIS2DW12_WHO_AM_I_VAL.
 * Does not start sampling — caller selects ODR + full-scale + mode
 * via @ref lis2dw12_set_accel.
 *
 * @return ALP_OK on success; ALP_ERR_IO on WHO_AM_I mismatch.
 */
alp_status_t lis2dw12_init(lis2dw12_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read WHO_AM_I for liveness checks.
 *
 * @param[in]  dev     Initialised context.
 * @param[out] id_out  Receives WHO_AM_I (expected @ref LIS2DW12_WHO_AM_I_VAL).
 * @return ALP_OK on success; non-zero alp_status_t on bad args or I2C error.
 */
alp_status_t lis2dw12_read_id(lis2dw12_t *dev, uint8_t *id_out);

/**
 * @brief Configure ODR + full-scale + power mode in a single call.
 *
 * Starts (or stops, for @ref LIS2DW12_ODR_OFF) sampling.  Caches @p fs and
 * @p mode in the context for later scaling.
 *
 * @param[in,out] dev   Initialised context.
 * @param[in]     odr   Output data rate.
 * @param[in]     fs    Full-scale range.
 * @param[in]     mode  Power/resolution mode.
 * @return ALP_OK on success; non-zero alp_status_t on bad args or I2C error.
 */
alp_status_t
lis2dw12_set_accel(lis2dw12_t *dev, lis2dw12_odr_t odr, lis2dw12_fs_t fs, lis2dw12_mode_t mode);

/**
 * @brief Read the current accelerometer sample (raw int16 counts).
 *
 * Counts are left-justified; convert to g using the configured full-scale.
 *
 * @param[in]  dev  Initialised context.
 * @param[out] out  Receives the three-axis raw sample.
 * @return ALP_OK on success; non-zero alp_status_t on bad args or I2C error.
 */
alp_status_t lis2dw12_read_accel(lis2dw12_t *dev, lis2dw12_axes_t *out);

/**
 * @brief Read the on-die temperature sensor (raw int16; 8-bit resolution).
 *
 * @param[in]  dev       Initialised context.
 * @param[out] temp_raw  Receives the raw temperature count.
 * @return ALP_OK on success; non-zero alp_status_t on bad args or I2C error.
 */
alp_status_t lis2dw12_read_temp(lis2dw12_t *dev, int16_t *temp_raw);

/**
 * @brief Release the driver context.  Does not power down the chip.
 *
 * @param[in,out] dev  Context to release (may be NULL).
 */
void lis2dw12_deinit(lis2dw12_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_LIS2DW12_H */
