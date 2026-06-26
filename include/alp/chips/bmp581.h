/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bmp581.h
 * @brief Bosch BMP581 ultra-low-power barometric pressure sensor.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Public surface consumed by alp-studio block `blk_baro_bmp581`.
 * Symbols carry the chip's natural prefix `bmp581_*` — no `alp_`.
 *
 * The BMP581 is one of the on-board sensors on the E1M EVK
 * (UG-E1M-001) alongside the IMUs.  Compared to the classic
 * BMP280 / BME280, the BMP581 returns **already-compensated**
 * 24-bit pressure (in 1/64 Pa) and 24-bit temperature (in
 * 1/65536 °C) -- no per-die calibration block to read or apply.
 *
 * v0.2 scope: I²C only, ODR + OSR config, raw 24-bit reads.
 * SPI lands in v0.3.
 *
 * Datasheet: Bosch BMP581 (BST-BMP581-DS004 v1.7, Mar 2024).
 */

#ifndef ALP_CHIPS_BMP581_H
#define ALP_CHIPS_BMP581_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default 7-bit I²C addresses (CSB strap selects). */
#define BMP581_I2C_ADDR_LOW  0x46 /**< CSB strapped low. */
#define BMP581_I2C_ADDR_HIGH 0x47 /**< CSB strapped high. */

/** CHIP_ID (register 0x01) value the chip returns. */
#define BMP581_CHIP_ID 0x50

/** @brief Oversampling rate (OSR_CONFIG bits[2:0] for T, bits[5:3] for P). */
typedef enum {
	BMP581_OSR_X1   = 0x0, /**< 1× oversampling. */
	BMP581_OSR_X2   = 0x1, /**< 2× oversampling. */
	BMP581_OSR_X4   = 0x2, /**< 4× oversampling. */
	BMP581_OSR_X8   = 0x3, /**< 8× oversampling. */
	BMP581_OSR_X16  = 0x4, /**< 16× oversampling. */
	BMP581_OSR_X32  = 0x5, /**< 32× oversampling. */
	BMP581_OSR_X64  = 0x6, /**< 64× oversampling. */
	BMP581_OSR_X128 = 0x7  /**< 128× oversampling (lowest noise). */
} bmp581_osr_t;

/** @brief Output data rate (ODR_CONFIG bits[6:2]). */
typedef enum {
	BMP581_ODR_240_HZ = 0x00, /**< 240 Hz. */
	BMP581_ODR_120_HZ = 0x01, /**< 120 Hz. */
	BMP581_ODR_50_HZ  = 0x07, /**< 50 Hz. */
	BMP581_ODR_25_HZ  = 0x0E, /**< 25 Hz. */
	BMP581_ODR_10_HZ  = 0x14, /**< 10 Hz. */
	BMP581_ODR_5_HZ   = 0x17, /**< 5 Hz. */
	BMP581_ODR_1_HZ   = 0x1C  /**< 1 Hz. */
} bmp581_odr_t;

/** @brief Power mode (ODR_CONFIG bits[1:0]). */
typedef enum {
	BMP581_MODE_STANDBY    = 0x0, /**< Idle; no conversions. */
	BMP581_MODE_NORMAL     = 0x1, /**< Periodic conversions at the configured ODR. */
	BMP581_MODE_FORCED     = 0x2, /**< Single conversion, then back to standby. */
	BMP581_MODE_CONTINUOUS = 0x3  /**< Free-running conversions (ignores ODR). */
} bmp581_mode_t;

/**
 * @brief Compensated-but-still-raw readings.
 *
 * Pressure: signed 24-bit, LSB = 1/64 Pa (so press_raw / 64.0 = Pa).
 * Temperature: signed 24-bit, LSB = 1/65536 °C.
 */
typedef struct {
	int32_t pressure_raw;    /**< Sign-extended from 24 → 32 bits; LSB = 1/64 Pa. */
	int32_t temperature_raw; /**< Sign-extended from 24 → 32 bits; LSB = 1/65536 °C. */
} bmp581_raw_t;

/** @brief Compensated readings in convenient integer units. */
typedef struct {
	int32_t pressure_pa;       /**< Pascals. */
	int32_t temperature_c1000; /**< Degrees C × 1000. */
} bmp581_compensated_t;

/** @brief Driver context.  Treat as opaque; populated by @ref bmp581_init. */
typedef struct {
	alp_i2c_t *bus;         /**< Caller-owned I²C bus the chip lives on. */
	uint8_t    addr;        /**< 7-bit I²C address. */
	bool       initialised; /**< True once @ref bmp581_init has verified CHIP_ID. */
} bmp581_t;

/**
 * @brief Bind a driver context to an open I²C bus and verify chip ID.
 *
 * Reads CHIP_ID and verifies it matches @ref BMP581_CHIP_ID.
 * Does not start sampling — caller selects ODR / OSR / mode via
 * @ref bmp581_set_sampling.  Ownership of @p bus stays with the caller.
 *
 * @param dev      Context to initialise.  Must be non-NULL.
 * @param bus      Open I²C bus.  Must be non-NULL.
 * @param i2c_addr 7-bit address (see BMP581_I2C_ADDR_*).
 * @return ALP_OK on success; ALP_ERR_IO on a CHIP_ID mismatch (chip absent or
 *         not a BMP581); otherwise an error/propagated I2C status.
 */
alp_status_t bmp581_init(bmp581_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read CHIP_ID for liveness checks.
 *
 * @param dev    Initialised context.  Must be non-NULL.
 * @param id_out Receives the CHIP_ID byte.  Must be non-NULL.
 * @return ALP_OK on success; otherwise an error status.
 */
alp_status_t bmp581_read_id(bmp581_t *dev, uint8_t *id_out);

/**
 * @brief Configure oversampling, ODR, and mode in one call.
 *
 * @param dev       Initialised context.  Must be non-NULL.
 * @param press_osr Pressure oversampling rate.
 * @param temp_osr  Temperature oversampling rate.
 * @param odr       Output data rate (used in periodic modes).
 * @param mode      Power/sampling mode.
 * @return ALP_OK on success; otherwise an error status.
 */
alp_status_t bmp581_set_sampling(bmp581_t     *dev,
                                 bmp581_osr_t  press_osr,
                                 bmp581_osr_t  temp_osr,
                                 bmp581_odr_t  odr,
                                 bmp581_mode_t mode);

/**
 * @brief Read the raw 24-bit P + T pair in one burst.
 *
 * @param dev Initialised context.  Must be non-NULL.
 * @param out Receives the sign-extended raw P/T sample.  Must be non-NULL.
 * @return ALP_OK on success; otherwise an error status.
 */
alp_status_t bmp581_read_raw(bmp581_t *dev, bmp581_raw_t *out);

/**
 * @brief Convert a raw reading into Pa + (°C × 1000).
 *
 * Pure helper — no chip access and no @p dev needed (the BMP581 returns
 * already-compensated values, so this only rescales the fixed-point LSBs).
 *
 * @param raw Raw sample to convert (e.g. from @ref bmp581_read_raw).  Non-NULL.
 * @param out Receives the rescaled result.  Must be non-NULL.
 * @return ALP_OK on success; ALP_ERR_INVAL on a NULL argument.
 */
alp_status_t bmp581_compensate(const bmp581_raw_t *raw, bmp581_compensated_t *out);

/**
 * @brief Soft-reset the chip (writes 0xB6 to CMD register).
 *
 * Re-run @ref bmp581_init afterwards.
 *
 * @param dev Initialised context.  Must be non-NULL.
 * @return ALP_OK on success; otherwise an error status.
 */
alp_status_t bmp581_soft_reset(bmp581_t *dev);

/**
 * @brief Release the driver context.
 *
 * Does not close the I²C bus or power down the chip.  NULL @p dev is a no-op.
 *
 * @param dev Context to release.  May be NULL.
 */
void bmp581_deinit(bmp581_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_BMP581_H */
