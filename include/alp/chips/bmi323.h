/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bmi323.h
 * @brief Bosch BMI323 6-axis IMU driver.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Public surface consumed by alp-studio block `blk_imu_bmi323`.
 * Symbols carry the chip's natural prefix `bmi323_*` — no `alp_`.
 *
 * The BMI323 is the second on-board IMU on the E1M EVK alongside
 * the ICM-42670-P; apps that want sensor fusion or redundancy can
 * read both.  Notable quirks:
 *
 *   - 16-bit register addressing (most IMUs use 8-bit).
 *   - Each register read returns a 2-byte dummy prefix that
 *     callers must skip (Bosch quirk for SPI alignment, also
 *     applies on I²C for consistency).
 *
 * v0.2 scope: I²C only, raw accel/gyro/temp reads, ODR + FS config.
 * v0.3 adds the on-chip Bosch fusion engine + virtual sensors.
 *
 * Datasheet: Bosch BMI323 (BST-BMI323-DS000 v1.5, Mar 2024).
 */

#ifndef ALP_CHIPS_BMI323_H
#define ALP_CHIPS_BMI323_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default 7-bit I2C addresses (SDO strap selects). */
#define BMI323_I2C_ADDR_LOW  0x68 /**< SDO strapped low. */
#define BMI323_I2C_ADDR_HIGH 0x69 /**< SDO strapped high. */

/** CHIP_ID (register 0x00) value. */
#define BMI323_CHIP_ID 0x43

/** @brief Output data rate (ACC_CONF / GYR_CONF bits[3:0]). */
typedef enum {
	BMI323_ODR_0_78125_HZ = 0x1, /**< 0.78125 Hz. */
	BMI323_ODR_1_5625_HZ  = 0x2, /**< 1.5625 Hz. */
	BMI323_ODR_3_125_HZ   = 0x3, /**< 3.125 Hz. */
	BMI323_ODR_6_25_HZ    = 0x4, /**< 6.25 Hz. */
	BMI323_ODR_12_5_HZ    = 0x5, /**< 12.5 Hz. */
	BMI323_ODR_25_HZ      = 0x6, /**< 25 Hz. */
	BMI323_ODR_50_HZ      = 0x7, /**< 50 Hz. */
	BMI323_ODR_100_HZ     = 0x8, /**< 100 Hz. */
	BMI323_ODR_200_HZ     = 0x9, /**< 200 Hz. */
	BMI323_ODR_400_HZ     = 0xA, /**< 400 Hz. */
	BMI323_ODR_800_HZ     = 0xB, /**< 800 Hz. */
	BMI323_ODR_1600_HZ    = 0xC, /**< 1600 Hz. */
	BMI323_ODR_3200_HZ    = 0xD, /**< 3200 Hz (accel only). */
	BMI323_ODR_6400_HZ    = 0xE  /**< 6400 Hz (accel only). */
} bmi323_odr_t;

/** @brief Accelerometer full-scale range (ACC_CONF bits[6:4]). */
typedef enum {
	BMI323_ACCEL_FS_2G  = 0x0, /**< ±2 g. */
	BMI323_ACCEL_FS_4G  = 0x1, /**< ±4 g. */
	BMI323_ACCEL_FS_8G  = 0x2, /**< ±8 g. */
	BMI323_ACCEL_FS_16G = 0x3  /**< ±16 g. */
} bmi323_accel_fs_t;

/** @brief Gyroscope full-scale range (GYR_CONF bits[6:4]). */
typedef enum {
	BMI323_GYRO_FS_125_DPS  = 0x0, /**< ±125 °/s. */
	BMI323_GYRO_FS_250_DPS  = 0x1, /**< ±250 °/s. */
	BMI323_GYRO_FS_500_DPS  = 0x2, /**< ±500 °/s. */
	BMI323_GYRO_FS_1000_DPS = 0x3, /**< ±1000 °/s. */
	BMI323_GYRO_FS_2000_DPS = 0x4  /**< ±2000 °/s. */
} bmi323_gyro_fs_t;

/** @brief Three-axis sample, raw 16-bit signed counts. */
typedef struct {
	int16_t x; /**< X-axis raw count. */
	int16_t y; /**< Y-axis raw count. */
	int16_t z; /**< Z-axis raw count. */
} bmi323_axes_t;

/** @brief Driver context.  Treat as opaque; populated by @ref bmi323_init. */
typedef struct {
	alp_i2c_t        *bus;         /**< Caller-owned I2C bus the chip lives on. */
	uint8_t           addr;        /**< 7-bit I2C address. */
	bmi323_accel_fs_t accel_fs;    /**< Last accel range set (for scaling raw counts). */
	bmi323_gyro_fs_t  gyro_fs;     /**< Last gyro range set (for scaling raw counts). */
	bool              initialised; /**< True once @ref bmi323_init has verified CHIP_ID. */
} bmi323_t;

/**
 * @brief Bind a driver context to an open I²C bus and verify chip ID.
 *
 * The BMI323 uses 16-bit register addressing -- the wrapper writes
 * a 2-byte address pointer for every transaction.  Reads return
 * a 2-byte dummy prefix that the wrapper strips internally.
 *
 * @param dev      Context to initialise.  Must be non-NULL.
 * @param bus      Open I²C bus.  Caller retains ownership.  Must be non-NULL.
 * @param i2c_addr 7-bit address (see BMI323_I2C_ADDR_*).
 * @return ALP_OK / ALP_ERR_INVAL (NULL arg) / ALP_ERR_IO (wrong CHIP_ID).
 */
alp_status_t bmi323_init(bmi323_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read CHIP_ID for liveness checks.
 *
 * @param dev    Initialised context.  Must be non-NULL.
 * @param id_out Receives the CHIP_ID byte.  Must be non-NULL.
 * @return ALP_OK on success; otherwise an error status.
 */
alp_status_t bmi323_read_id(bmi323_t *dev, uint8_t *id_out);

/**
 * @brief Configure accelerometer ODR + full-scale range.
 *
 * Records @p fs in @p dev so later raw reads can be scaled.
 *
 * @param dev Initialised context.  Must be non-NULL.
 * @param odr Output data rate.
 * @param fs  Full-scale range.
 * @return ALP_OK on success; otherwise an error status.
 */
alp_status_t bmi323_set_accel(bmi323_t *dev, bmi323_odr_t odr, bmi323_accel_fs_t fs);

/**
 * @brief Configure gyroscope ODR + full-scale range.
 *
 * Records @p fs in @p dev so later raw reads can be scaled.
 *
 * @param dev Initialised context.  Must be non-NULL.
 * @param odr Output data rate.
 * @param fs  Full-scale range.
 * @return ALP_OK on success; otherwise an error status.
 */
alp_status_t bmi323_set_gyro(bmi323_t *dev, bmi323_odr_t odr, bmi323_gyro_fs_t fs);

/**
 * @brief Read the current accelerometer sample (raw int16 counts).
 *
 * @param dev Initialised context.  Must be non-NULL.
 * @param out Receives the X/Y/Z raw sample.  Must be non-NULL.
 * @return ALP_OK on success; otherwise an error status.
 */
alp_status_t bmi323_read_accel(bmi323_t *dev, bmi323_axes_t *out);

/**
 * @brief Read the current gyroscope sample (raw int16 counts).
 *
 * @param dev Initialised context.  Must be non-NULL.
 * @param out Receives the X/Y/Z raw sample.  Must be non-NULL.
 * @return ALP_OK on success; otherwise an error status.
 */
alp_status_t bmi323_read_gyro(bmi323_t *dev, bmi323_axes_t *out);

/**
 * @brief Read the on-die temperature sensor (raw int16; 0 count = 23.0 °C).
 *
 * @param dev      Initialised context.  Must be non-NULL.
 * @param temp_raw Receives the raw signed temperature count.  Must be non-NULL.
 * @return ALP_OK on success; otherwise an error status.
 */
alp_status_t bmi323_read_temp(bmi323_t *dev, int16_t *temp_raw);

/**
 * @brief Release the driver context.
 *
 * Does not close the I²C bus or power down the chip.  NULL @p dev is a no-op.
 *
 * @param dev Context to release.  May be NULL.
 */
void bmi323_deinit(bmi323_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_BMI323_H */
