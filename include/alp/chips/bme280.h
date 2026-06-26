/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bme280.h
 * @brief Bosch BME280 combined temperature / humidity / pressure sensor driver.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Public surface consumed by alp-studio block `blk_env_bme280`.  Symbols
 * carry the chip's natural prefix `bme280_*` — no `alp_`.
 *
 * v0.2 scope: I2C only, single-shot ("forced") and continuous ("normal")
 * sampling modes, raw register reads + compensation helper that lifts the
 * 20/20/16-bit raw output into degrees-celsius (×100), pascals, and
 * %RH (×1024) using the chip's per-die calibration coefficients.  SPI
 * arrives in v0.3.
 *
 * Datasheet: Bosch BME280 (BST-BME280-DS002 v1.6, May 2022).
 */

#ifndef ALP_CHIPS_BME280_H
#define ALP_CHIPS_BME280_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default 7-bit I2C addresses (SDO pin selects). */
#define BME280_I2C_ADDR_LOW  0x76 /**< SDO tied low. */
#define BME280_I2C_ADDR_HIGH 0x77 /**< SDO tied high. */

/** WHO_AM_I (CHIP_ID) register value the chip returns. */
#define BME280_CHIP_ID 0x60

/** @brief Oversampling settings.  Same field encoding for each of T / P / H. */
typedef enum {
	BME280_OVERSAMPLING_SKIPPED = 0x0, /**< Channel disabled (output held at reset). */
	BME280_OVERSAMPLING_X1      = 0x1, /**< 1× oversampling. */
	BME280_OVERSAMPLING_X2      = 0x2, /**< 2× oversampling. */
	BME280_OVERSAMPLING_X4      = 0x3, /**< 4× oversampling. */
	BME280_OVERSAMPLING_X8      = 0x4, /**< 8× oversampling. */
	BME280_OVERSAMPLING_X16     = 0x5  /**< 16× oversampling (lowest noise). */
} bme280_oversampling_t;

/** @brief Sampling mode (CTRL_MEAS bits[1:0]). */
typedef enum {
	BME280_MODE_SLEEP  = 0x0, /**< Idle; no conversions, lowest power. */
	BME280_MODE_FORCED = 0x1, /**< Single conversion, then return to sleep. */
	BME280_MODE_NORMAL = 0x3  /**< Continuous conversion cycling at standby_t. */
} bme280_mode_t;

/** @brief Standby duration between conversions in BME280_MODE_NORMAL. */
typedef enum {
	BME280_STANDBY_0_5_MS  = 0x0, /**< 0.5 ms standby. */
	BME280_STANDBY_62_5_MS = 0x1, /**< 62.5 ms standby. */
	BME280_STANDBY_125_MS  = 0x2, /**< 125 ms standby. */
	BME280_STANDBY_250_MS  = 0x3, /**< 250 ms standby. */
	BME280_STANDBY_500_MS  = 0x4, /**< 500 ms standby. */
	BME280_STANDBY_1000_MS = 0x5, /**< 1000 ms standby. */
	BME280_STANDBY_10_MS   = 0x6, /**< 10 ms standby. */
	BME280_STANDBY_20_MS   = 0x7  /**< 20 ms standby. */
} bme280_standby_t;

/** @brief IIR filter coefficient (CONFIG bits[4:2]). */
typedef enum {
	BME280_FILTER_OFF = 0x0, /**< Filter disabled. */
	BME280_FILTER_2   = 0x1, /**< Coefficient 2. */
	BME280_FILTER_4   = 0x2, /**< Coefficient 4. */
	BME280_FILTER_8   = 0x3, /**< Coefficient 8. */
	BME280_FILTER_16  = 0x4  /**< Coefficient 16 (heaviest smoothing). */
} bme280_filter_t;

/**
 * @brief Raw uncompensated readings.
 *
 * 20-bit pressure & temperature stored in the low bits of the int32 fields;
 * 16-bit humidity in the low bits of the uint32 field.  Use
 * @ref bme280_compensate to obtain physical units.
 */
typedef struct {
	int32_t  pressure_raw;    /**< 20-bit, range 0x00000–0xFFFFF. */
	int32_t  temperature_raw; /**< 20-bit, range 0x00000–0xFFFFF. */
	uint32_t humidity_raw;    /**< 16-bit, range 0x0000–0xFFFF. */
} bme280_raw_t;

/** @brief Compensated readings in fixed-point integer units. */
typedef struct {
	int32_t  temperature_c100;   /**< Degrees C × 100 (e.g. 2123 = 21.23 °C). */
	uint32_t pressure_pa;        /**< Pascals (e.g. 101325). */
	uint32_t humidity_milli_pct; /**< %RH × 1024 (raw Bosch fixed-point Q22.10). */
} bme280_compensated_t;

/**
 * @brief Per-die calibration coefficients pulled from chip NVM during init.
 *
 * Opaque Bosch trim values; consumed by @ref bme280_compensate and not meant
 * for direct interpretation.  Field names mirror the datasheet's dig_T/P/H.
 */
typedef struct {
	uint16_t T1;                             /**< Temperature trim T1. */
	int16_t  T2;                             /**< Temperature trim T2. */
	int16_t  T3;                             /**< Temperature trim T3. */
	uint16_t P1;                             /**< Pressure trim P1. */
	int16_t  P2, P3, P4, P5, P6, P7, P8, P9; /**< Pressure trims P2..P9. */
	uint8_t  H1;                             /**< Humidity trim H1. */
	int16_t  H2;                             /**< Humidity trim H2. */
	uint8_t  H3;                             /**< Humidity trim H3. */
	int16_t  H4, H5;                         /**< Humidity trims H4, H5. */
	int8_t   H6;                             /**< Humidity trim H6. */
} bme280_calib_t;

/** @brief Driver context.  Treat as opaque; populated by @ref bme280_init. */
typedef struct {
	alp_i2c_t     *bus;         /**< Caller-owned I2C bus the chip lives on. */
	uint8_t        addr;        /**< 7-bit I2C address. */
	bme280_calib_t calib;       /**< Per-die trim loaded at init. */
	int32_t        t_fine;      /**< Carry-over from temperature compensation. */
	bool           initialised; /**< True once @ref bme280_init has verified CHIP_ID. */
} bme280_t;

/**
 * @brief Bind a driver context to an open I2C bus.
 *
 * Reads CHIP_ID and verifies it matches @ref BME280_CHIP_ID, then loads
 * the per-die calibration coefficients into @p dev.  Leaves the
 * oversampling / mode registers untouched — call
 * @ref bme280_set_sampling next.
 *
 * @param dev      Context to initialise.  Must be non-NULL.
 * @param bus      Open I2C bus.  Caller retains ownership.  Must be non-NULL.
 * @param i2c_addr 7-bit address (see BME280_I2C_ADDR_*).
 * @return ALP_OK on success; ALP_ERR_IO on a CHIP_ID mismatch
 *         (chip absent or not a BME280); otherwise the propagated I2C status.
 */
alp_status_t bme280_init(bme280_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read CHIP_ID for liveness checks post-init.
 *
 * @param dev    Initialised context.  Must be non-NULL.
 * @param id_out Receives the CHIP_ID byte.  Must be non-NULL.
 * @return ALP_OK on success; an error status on a NULL/uninitialised argument
 *         or I2C failure.
 */
alp_status_t bme280_read_id(bme280_t *dev, uint8_t *id_out);

/**
 * @brief Configure sampling parameters.
 *
 * Writes CTRL_HUM, CTRL_MEAS, and CONFIG in the order the datasheet
 * mandates (humidity first, then a CTRL_MEAS write to latch hum + start
 * the conversion).  Mode is the low two bits of CTRL_MEAS.
 *
 * @param dev     Initialised context.  Must be non-NULL.
 * @param t_os    Temperature oversampling.
 * @param p_os    Pressure oversampling.
 * @param h_os    Humidity oversampling.
 * @param mode    Power/sampling mode.
 * @param standby Standby duration (only used in BME280_MODE_NORMAL).
 * @param filter  IIR filter coefficient.
 * @return ALP_OK on success; otherwise an error status (NULL/uninitialised
 *         @p dev or an I2C failure).
 */
alp_status_t bme280_set_sampling(bme280_t             *dev,
                                 bme280_oversampling_t t_os,
                                 bme280_oversampling_t p_os,
                                 bme280_oversampling_t h_os,
                                 bme280_mode_t         mode,
                                 bme280_standby_t      standby,
                                 bme280_filter_t       filter);

/**
 * @brief Read the raw 20/20/16-bit conversion result in one burst.
 *
 * @param dev Initialised context.  Must be non-NULL.
 * @param out Receives the raw P/T/H sample.  Must be non-NULL.
 * @return ALP_OK on success; otherwise an error status.
 */
alp_status_t bme280_read_raw(bme280_t *dev, bme280_raw_t *out);

/**
 * @brief Lift raw readings to physical units using @p dev's calibration.
 *
 * Stateful — temperature compensation populates `t_fine` inside @p dev
 * and the pressure / humidity legs read it back, so callers must always
 * compensate temperature first (this helper does so internally).  Not
 * reentrant on a shared @p dev.
 *
 * @param dev Initialised context (supplies calibration + t_fine).  Non-NULL.
 * @param raw Raw sample to convert (e.g. from @ref bme280_read_raw).  Non-NULL.
 * @param out Receives the compensated result.  Must be non-NULL.
 * @return ALP_OK on success; otherwise an error status.
 */
alp_status_t bme280_compensate(bme280_t *dev, const bme280_raw_t *raw, bme280_compensated_t *out);

/**
 * @brief Issue a soft reset (writes 0xB6 to register RESET).
 *
 * Chip reloads calibration from NVM on next access; re-run @ref bme280_init
 * afterwards.
 *
 * @param dev Initialised context.  Must be non-NULL.
 * @return ALP_OK on success; otherwise an error status.
 */
alp_status_t bme280_soft_reset(bme280_t *dev);

/**
 * @brief Release the driver context.
 *
 * Does not close the I2C bus or power down the chip.  NULL @p dev is a no-op.
 *
 * @param dev Context to release.  May be NULL.
 */
void bme280_deinit(bme280_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_BME280_H */
