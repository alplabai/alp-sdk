/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lsm6dso.h
 * @brief STMicroelectronics LSM6DSO 6-axis IMU driver.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Public surface consumed by alp-studio block `blk_imu_lsm6dso`.
 * Symbols carry the chip's natural prefix `lsm6dso_*` — no `alp_`
 * (chip drivers are bindings to third-party silicon).
 *
 * I2C-only in v0.1.  SPI lands in v0.2 along with `lsm6dso_init_spi`.
 *
 * Datasheet: STMicroelectronics LSM6DSO (DocID032086).
 */

#ifndef ALP_CHIPS_LSM6DSO_H
#define ALP_CHIPS_LSM6DSO_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default 7-bit I2C addresses (SDO/SA0 pin selects). */
#define LSM6DSO_I2C_ADDR_LOW  0x6A /**< SDO/SA0 tied low. */
#define LSM6DSO_I2C_ADDR_HIGH 0x6B /**< SDO/SA0 tied high. */

/** WHO_AM_I register value the chip returns for self-identification. */
#define LSM6DSO_WHO_AM_I_VAL 0x6C

/** @brief Accelerometer / gyroscope output data rates (CTRL1/CTRL2 ODR field). */
typedef enum {
	LSM6DSO_ODR_OFF     = 0x0, /**< Sensor powered down (no samples). */
	LSM6DSO_ODR_12_5_HZ = 0x1, /**< 12.5 Hz. */
	LSM6DSO_ODR_26_HZ   = 0x2, /**< 26 Hz. */
	LSM6DSO_ODR_52_HZ   = 0x3, /**< 52 Hz. */
	LSM6DSO_ODR_104_HZ  = 0x4, /**< 104 Hz. */
	LSM6DSO_ODR_208_HZ  = 0x5, /**< 208 Hz. */
	LSM6DSO_ODR_416_HZ  = 0x6, /**< 416 Hz. */
	LSM6DSO_ODR_833_HZ  = 0x7, /**< 833 Hz. */
	LSM6DSO_ODR_1660_HZ = 0x8, /**< 1.66 kHz. */
	LSM6DSO_ODR_3330_HZ = 0x9, /**< 3.33 kHz. */
	LSM6DSO_ODR_6660_HZ = 0xA  /**< 6.66 kHz. */
} lsm6dso_odr_t;

/** @brief Accelerometer full-scale range (sets counts-to-g sensitivity). */
typedef enum {
	LSM6DSO_ACCEL_FS_2G  = 0x0, /**< +/-2 g. */
	LSM6DSO_ACCEL_FS_16G = 0x1, /**< +/-16 g. */
	LSM6DSO_ACCEL_FS_4G  = 0x2, /**< +/-4 g. */
	LSM6DSO_ACCEL_FS_8G  = 0x3  /**< +/-8 g. */
} lsm6dso_accel_fs_t;

/** @brief Gyroscope full-scale range in degrees per second. */
typedef enum {
	LSM6DSO_GYRO_FS_250_DPS  = 0x0, /**< +/-250 dps. */
	LSM6DSO_GYRO_FS_500_DPS  = 0x1, /**< +/-500 dps. */
	LSM6DSO_GYRO_FS_1000_DPS = 0x2, /**< +/-1000 dps. */
	LSM6DSO_GYRO_FS_2000_DPS = 0x3  /**< +/-2000 dps. */
} lsm6dso_gyro_fs_t;

/** @brief Three-axis sample as raw 16-bit signed counts (scale set by full-scale range). */
typedef struct {
	int16_t x; /**< X-axis count. */
	int16_t y; /**< Y-axis count. */
	int16_t z; /**< Z-axis count. */
} lsm6dso_axes_t;

/** @brief Driver context for one LSM6DSO. Treat as opaque. */
typedef struct {
	alp_i2c_t         *bus;         /**< Borrowed I2C bus; owned by the caller, not freed here. */
	uint8_t            addr;        /**< 7-bit I2C address. */
	lsm6dso_accel_fs_t accel_fs;    /**< Last accelerometer full-scale set via lsm6dso_set_accel. */
	lsm6dso_gyro_fs_t  gyro_fs;     /**< Last gyroscope full-scale set via lsm6dso_set_gyro. */
	bool               initialised; /**< True once lsm6dso_init has validated WHO_AM_I. */
} lsm6dso_t;

/**
 * @brief Bind a driver context to an open I2C bus.
 *
 * Reads WHO_AM_I and verifies it matches @ref LSM6DSO_WHO_AM_I_VAL.
 * Does not touch ODR or full-scale registers — the caller selects
 * those next via @ref lsm6dso_set_accel and @ref lsm6dso_set_gyro.
 *
 * @return ALP_OK on success, or an error from the underlying I2C
 *         transfer.  Returns ALP_ERR_IO when the WHO_AM_I check
 *         fails (chip not present or not LSM6DSO).
 */
alp_status_t lsm6dso_init(lsm6dso_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read the WHO_AM_I register. Useful for liveness checks post-init.
 *
 * @param dev    Initialised context.
 * @param id_out Receives the register value (expected @ref LSM6DSO_WHO_AM_I_VAL).
 * @return ALP_OK on success, or an error from the I2C transfer.
 */
alp_status_t lsm6dso_read_id(lsm6dso_t *dev, uint8_t *id_out);

/**
 * @brief Configure accelerometer output data rate and full-scale range.
 *
 * Also records @p fs in the context so reads can be scaled consistently.
 *
 * @param dev Initialised context.
 * @param odr Output data rate; LSM6DSO_ODR_OFF powers the accelerometer down.
 * @param fs  Full-scale range.
 * @return ALP_OK on success, or an error from the I2C transfer.
 */
alp_status_t lsm6dso_set_accel(lsm6dso_t *dev, lsm6dso_odr_t odr, lsm6dso_accel_fs_t fs);

/**
 * @brief Configure gyroscope output data rate and full-scale range.
 *
 * Also records @p fs in the context so reads can be scaled consistently.
 *
 * @param dev Initialised context.
 * @param odr Output data rate; LSM6DSO_ODR_OFF powers the gyroscope down.
 * @param fs  Full-scale range.
 * @return ALP_OK on success, or an error from the I2C transfer.
 */
alp_status_t lsm6dso_set_gyro(lsm6dso_t *dev, lsm6dso_odr_t odr, lsm6dso_gyro_fs_t fs);

/**
 * @brief Read the current accelerometer sample as raw int16 counts.
 *
 * @param dev Initialised context with the accelerometer enabled.
 * @param out Receives the three-axis sample; scale depends on the active full-scale.
 * @return ALP_OK on success, or an error from the I2C transfer.
 */
alp_status_t lsm6dso_read_accel(lsm6dso_t *dev, lsm6dso_axes_t *out);

/**
 * @brief Read the current gyroscope sample as raw int16 counts.
 *
 * @param dev Initialised context with the gyroscope enabled.
 * @param out Receives the three-axis sample; scale depends on the active full-scale.
 * @return ALP_OK on success, or an error from the I2C transfer.
 */
alp_status_t lsm6dso_read_gyro(lsm6dso_t *dev, lsm6dso_axes_t *out);

/**
 * @brief Read the on-die temperature sensor.
 *
 * Convert with: degrees_C = 25 + temp_raw / 256.
 *
 * @param dev      Initialised context.
 * @param temp_raw Receives the raw signed count (LSB ~= 1/256 degree C).
 * @return ALP_OK on success, or an error from the I2C transfer.
 */
alp_status_t lsm6dso_read_temp(lsm6dso_t *dev, int16_t *temp_raw);

/**
 * @brief Release the driver context. Does not power down the chip nor close
 *        the borrowed I2C bus.
 *
 * @param dev Context to release; NULL is ignored.
 */
void lsm6dso_deinit(lsm6dso_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_LSM6DSO_H */
