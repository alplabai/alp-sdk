/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file icm42670.h
 * @brief TDK InvenSense ICM-42670-P 6-axis IMU driver.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Public surface consumed by alp-studio block `blk_imu_icm42670`.
 * Symbols carry the chip's natural prefix `icm42670_*` — no `alp_`
 * (chip drivers are bindings to third-party silicon).
 *
 * The ICM-42670-P is one of the on-board IMUs on the E1M EVK
 * (UG-E1M-001).  Compared to the LSM6DSO it adds APEX (algorithm
 * processing engine) features (pedometer, tilt detection, freefall)
 * driven by an internal DMP, but this v0.2 driver covers only the
 * raw accel/gyro/temperature path; APEX integration arrives in v0.3.
 *
 * I²C-only in v0.2.  SPI lands alongside the v0.3 DMP support.
 *
 * Datasheet: TDK InvenSense ICM-42670-P (DS-000451 v1.6, Apr 2024).
 */

#ifndef ALP_CHIPS_ICM42670_H
#define ALP_CHIPS_ICM42670_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default 7-bit I2C addresses (AP_AD0 strap selects). */
#define ICM42670_I2C_ADDR_LOW  0x68 /**< AP_AD0 tied low. */
#define ICM42670_I2C_ADDR_HIGH 0x69 /**< AP_AD0 tied high. */

/** WHO_AM_I (register 0x75) value the chip returns. */
#define ICM42670_WHO_AM_I_VAL 0x67

/**
 * @brief Accelerometer / gyroscope output data rates.
 *
 * Register field codes for the PWR_MGMT0 / ACCEL_CONFIG0 / GYRO_CONFIG0 ODR
 * fields.  Rates flagged "low-power only" are valid only when the matching
 * sensor runs in low-power (LP) mode; the remaining rates work in both
 * low-noise (LN) and LP modes.
 */
typedef enum {
	ICM42670_ODR_OFF       = 0x0, /**< Output disabled (sensor in standby). */
	ICM42670_ODR_1_5625_HZ = 0xF, /**< 1.5625 Hz (low-power only). */
	ICM42670_ODR_3_125_HZ  = 0xE, /**< 3.125 Hz (low-power only). */
	ICM42670_ODR_6_25_HZ   = 0xD, /**< 6.25 Hz (low-power only). */
	ICM42670_ODR_12_5_HZ   = 0xC, /**< 12.5 Hz. */
	ICM42670_ODR_25_HZ     = 0xB, /**< 25 Hz. */
	ICM42670_ODR_50_HZ     = 0xA, /**< 50 Hz. */
	ICM42670_ODR_100_HZ    = 0x8, /**< 100 Hz. */
	ICM42670_ODR_200_HZ    = 0x7, /**< 200 Hz. */
	ICM42670_ODR_400_HZ    = 0x6, /**< 400 Hz. */
	ICM42670_ODR_800_HZ    = 0x5, /**< 800 Hz. */
	ICM42670_ODR_1600_HZ   = 0x4  /**< 1600 Hz (low-noise only). */
} icm42670_odr_t;

/**
 * @brief Accelerometer full-scale range (ACCEL_CONFIG0 bits[6:5]).
 *
 * A wider range trades resolution for headroom; the LSB-per-g figure is the
 * sensitivity to apply when converting raw counts to acceleration.
 */
typedef enum {
	ICM42670_ACCEL_FS_16G = 0x0, /**< ±16 g (2048 LSB/g). */
	ICM42670_ACCEL_FS_8G  = 0x1, /**< ±8 g (4096 LSB/g). */
	ICM42670_ACCEL_FS_4G  = 0x2, /**< ±4 g (8192 LSB/g). */
	ICM42670_ACCEL_FS_2G  = 0x3  /**< ±2 g (16384 LSB/g). */
} icm42670_accel_fs_t;

/**
 * @brief Gyroscope full-scale range (GYRO_CONFIG0 bits[6:5]).
 *
 * The LSB-per-(°/s) figure is the sensitivity to apply when converting raw
 * counts to angular rate.
 */
typedef enum {
	ICM42670_GYRO_FS_2000_DPS = 0x0, /**< ±2000 °/s (16.4 LSB per °/s). */
	ICM42670_GYRO_FS_1000_DPS = 0x1, /**< ±1000 °/s (32.8 LSB per °/s). */
	ICM42670_GYRO_FS_500_DPS  = 0x2, /**< ±500 °/s (65.5 LSB per °/s). */
	ICM42670_GYRO_FS_250_DPS  = 0x3  /**< ±250 °/s (131 LSB per °/s). */
} icm42670_gyro_fs_t;

/** @brief Three-axis sample, raw 16-bit signed counts (scale by current full-scale). */
typedef struct {
	int16_t x; /**< X-axis raw signed count. */
	int16_t y; /**< Y-axis raw signed count. */
	int16_t z; /**< Z-axis raw signed count. */
} icm42670_axes_t;

/** @brief Driver context.  Caller-allocated; treat fields as opaque. */
typedef struct {
	alp_i2c_t          *bus;         /**< Bound I²C bus (borrowed; not owned/closed by driver). */
	uint8_t             addr;        /**< 7-bit I²C address in use. */
	icm42670_accel_fs_t accel_fs;    /**< Cached accel full-scale (for count→g scaling). */
	icm42670_gyro_fs_t  gyro_fs;     /**< Cached gyro full-scale (for count→°/s scaling). */
	bool                initialised; /**< True once icm42670_init() verified WHO_AM_I. */
} icm42670_t;

/**
 * @brief Bind a driver context to an open I²C bus and verify chip ID.
 *
 * Reads WHO_AM_I and verifies it matches @ref ICM42670_WHO_AM_I_VAL.
 * Does not start sampling -- caller selects ODR + FS via
 * @ref icm42670_set_accel and @ref icm42670_set_gyro.
 *
 * @param dev       Caller-allocated context to initialise.
 * @param bus       Open I²C bus; borrowed, must outlive @p dev.
 * @param i2c_addr  7-bit address (@ref ICM42670_I2C_ADDR_LOW / _HIGH).
 * @return ALP_OK on success; ALP_ERR_IO on WHO_AM_I mismatch.
 */
alp_status_t icm42670_init(icm42670_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read WHO_AM_I for liveness checks.
 *
 * @param dev     Initialised context.
 * @param id_out  Receives the WHO_AM_I value (expected @ref ICM42670_WHO_AM_I_VAL).
 * @return ALP_OK on success; ALP_ERR_IO on bus failure.
 */
alp_status_t icm42670_read_id(icm42670_t *dev, uint8_t *id_out);

/**
 * @brief Configure accelerometer output data rate and full-scale range.
 *
 * @param dev  Initialised context.
 * @param odr  Output data rate (@ref ICM42670_ODR_OFF powers the accel down).
 * @param fs   Full-scale range; cached for later count→g scaling.
 * @return ALP_OK on success; ALP_ERR_IO on bus failure.
 */
alp_status_t icm42670_set_accel(icm42670_t *dev, icm42670_odr_t odr, icm42670_accel_fs_t fs);

/**
 * @brief Configure gyroscope output data rate and full-scale range.
 *
 * @param dev  Initialised context.
 * @param odr  Output data rate (@ref ICM42670_ODR_OFF powers the gyro down).
 * @param fs   Full-scale range; cached for later count→°/s scaling.
 * @return ALP_OK on success; ALP_ERR_IO on bus failure.
 */
alp_status_t icm42670_set_gyro(icm42670_t *dev, icm42670_odr_t odr, icm42670_gyro_fs_t fs);

/**
 * @brief Read the current accelerometer sample.
 *
 * @param dev  Initialised context with the accelerometer enabled.
 * @param out  Receives raw signed counts; scale by the configured full-scale.
 * @return ALP_OK on success; ALP_ERR_IO on bus failure.
 */
alp_status_t icm42670_read_accel(icm42670_t *dev, icm42670_axes_t *out);

/**
 * @brief Read the current gyroscope sample.
 *
 * @param dev  Initialised context with the gyroscope enabled.
 * @param out  Receives raw signed counts; scale by the configured full-scale.
 * @return ALP_OK on success; ALP_ERR_IO on bus failure.
 */
alp_status_t icm42670_read_gyro(icm42670_t *dev, icm42670_axes_t *out);

/**
 * @brief Read the on-die temperature sensor.
 *
 * @param dev       Initialised context.
 * @param temp_raw  Receives raw signed counts; °C ≈ (raw / 132.48) + 25.
 * @return ALP_OK on success; ALP_ERR_IO on bus failure.
 */
alp_status_t icm42670_read_temp(icm42670_t *dev, int16_t *temp_raw);

/**
 * @brief Release the driver context.
 *
 * Clears the context's initialised flag; does not power down the chip or
 * close the borrowed I²C bus.
 *
 * @param dev  Context to release (may be NULL).
 */
void icm42670_deinit(icm42670_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_ICM42670_H */
