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
 * v0.2 scope: I²C only, ODR + OSR config, raw 24-bit reads, data-ready
 * polling, and INT pin configuration (electrical config only -- this
 * driver does not know which board pin the INT line lands on).
 * SPI lands in v0.3.
 *
 * Datasheet: Bosch BMP581 (BST-BMP581-DS004-13 Rev 1.13).
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
#define BMP581_I2C_ADDR_LOW  0x46
#define BMP581_I2C_ADDR_HIGH 0x47

/** CHIP_ID (register 0x01) value the chip returns. */
#define BMP581_CHIP_ID 0x50

/** Oversampling rate (OSR_CONFIG bits[2:0] for T, bits[5:3] for P). */
typedef enum {
	BMP581_OSR_X1   = 0x0,
	BMP581_OSR_X2   = 0x1,
	BMP581_OSR_X4   = 0x2,
	BMP581_OSR_X8   = 0x3,
	BMP581_OSR_X16  = 0x4,
	BMP581_OSR_X32  = 0x5,
	BMP581_OSR_X64  = 0x6,
	BMP581_OSR_X128 = 0x7
} bmp581_osr_t;

/** Output data rate (ODR_CONFIG bits[6:2]).
 *  #2035: five of these seven codes were wrong -- picked as if the 32-code
 *  field were a linear divider, when BST-BMP581-DS004-13 §7.34 (p.65)'s ODR
 *  table is not. Verified against that table and cross-checked against
 *  Bosch's own BMP5-Sensor-API `bmp5_defs.h` (BMP5_ODR_*), which is
 *  generated from the same table. Actual rate for each code below:
 *  0x00=240.000 Hz, 0x08=120.000 Hz, 0x0F=50.056 Hz, 0x14=25.005 Hz,
 *  0x17=10.000 Hz, 0x18=5.000 Hz, 0x1C=1.000 Hz. A consumer selecting
 *  BMP581_ODR_5_HZ under the old (wrong) code silently got 10 Hz instead. */
typedef enum {
	BMP581_ODR_240_HZ = 0x00,
	BMP581_ODR_120_HZ = 0x08,
	BMP581_ODR_50_HZ  = 0x0F,
	BMP581_ODR_25_HZ  = 0x14,
	BMP581_ODR_10_HZ  = 0x17,
	BMP581_ODR_5_HZ   = 0x18,
	BMP581_ODR_1_HZ   = 0x1C
} bmp581_odr_t;

/** Power mode (ODR_CONFIG bits[1:0]). */
typedef enum {
	BMP581_MODE_STANDBY    = 0x0,
	BMP581_MODE_NORMAL     = 0x1,
	BMP581_MODE_FORCED     = 0x2,
	BMP581_MODE_CONTINUOUS = 0x3
} bmp581_mode_t;

/** INT pin drive stage (INT_CONFIG.int_od, reg 0x14 bit 2).
 *  BST-BMP581-DS004-13 §7.5 (p.52-53). */
typedef enum {
	BMP581_INT_PUSH_PULL  = 0x0, /**< Reset default is open-drain, not this. */
	BMP581_INT_OPEN_DRAIN = 0x1
} bmp581_int_drive_t;

/** INT pin active polarity (INT_CONFIG.int_pol, reg 0x14 bit 1).
 *  BST-BMP581-DS004-13 §7.5 (p.52-53). */
typedef enum { BMP581_INT_ACTIVE_LOW = 0x0, BMP581_INT_ACTIVE_HIGH = 0x1 } bmp581_int_polarity_t;

/** INT pin signalling mode (INT_CONFIG.int_mode, reg 0x14 bit 0).
 *  BST-BMP581-DS004-13 §7.5 (p.52-53). */
typedef enum { BMP581_INT_PULSED = 0x0, BMP581_INT_LATCHED = 0x1 } bmp581_int_mode_t;

/** INT_SOURCE (reg 0x15) enable bits -- OR together and pass as a mask
 *  to @ref bmp581_set_int_sources.  BST-BMP581-DS004-13 §7.6 (p.54). */
#define BMP581_INT_SRC_DRDY      (1u << 0) /**< drdy_data_reg_en. */
#define BMP581_INT_SRC_FIFO_FULL (1u << 1) /**< fifo_full_en.     */
#define BMP581_INT_SRC_FIFO_THS  (1u << 2) /**< fifo_ths_en.      */
#define BMP581_INT_SRC_OOR_P     (1u << 3) /**< oor_p_en (pressure out-of-range). */

/** Compensated-but-still-raw readings.
 *  Pressure: signed 24-bit, LSB = 1/64 Pa (so press_raw / 64.0 = Pa).
 *  Temperature: signed 24-bit, LSB = 1/65536 °C. */
typedef struct {
	int32_t pressure_raw; /**< Sign-extended from 24 → 32 bits. */
	int32_t temperature_raw;
} bmp581_raw_t;

/** Compensated readings in convenient integer units. */
typedef struct {
	int32_t pressure_pa;       /**< Pascals. */
	int32_t temperature_c1000; /**< Degrees C × 1000. */
} bmp581_compensated_t;

/** Driver context.  Treat as opaque. */
typedef struct {
	alp_i2c_t *bus;
	uint8_t    addr;
	bool       initialised;
} bmp581_t;

/**
 * @brief Bind a driver context to an open I²C bus and verify chip ID.
 *
 * Reads CHIP_ID and verifies it matches @ref BMP581_CHIP_ID.
 * Does not start sampling — caller selects ODR / OSR / mode via
 * @ref bmp581_set_sampling.
 */
alp_status_t bmp581_init(bmp581_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** Read CHIP_ID for liveness checks. */
alp_status_t bmp581_read_id(bmp581_t *dev, uint8_t *id_out);

/**
 * @brief Configure oversampling, ODR, and mode in one call.
 *
 * #2035: this always transitions the part through STANDBY first (with
 * deep_dis set) before writing OSR_CONFIG / ODR_CONFIG, per
 * BST-BMP581-DS004-13 §4.3 (p.16) / §4.3.8 (p.18) -- writing those
 * registers straight out of DEEP STANDBY (the power-on default) is
 * silently discarded, and the config would never take effect. The
 * STANDBY step costs one extra register write plus a ~3 ms delay.
 *
 * A single-shot @ref BMP581_MODE_FORCED conversion is not complete the
 * instant this call returns -- the chip needs a real sampling window.
 * Either poll @ref bmp581_data_ready before reading, or wait at least
 * the conversion time yourself: BST-BMP581-DS004-13 tconv is ~1.0 ms
 * typ per channel at OSR 1x (p.12), so a forced pressure+temperature
 * measurement at OSR 1x is roughly 2.0 ms end to end, not the ~5x
 * margin a 5 ms guess would suggest -- 5 ms is only ~2.5x that figure.
 * Higher OSR settings scale tconv up proportionally (see the datasheet's
 * OSR table); re-derive the wait for any OSR above x1.
 */
alp_status_t bmp581_set_sampling(bmp581_t     *dev,
                                 bmp581_osr_t  press_osr,
                                 bmp581_osr_t  temp_osr,
                                 bmp581_odr_t  odr,
                                 bmp581_mode_t mode);

/** Read the raw 24-bit P + T pair in one burst. */
alp_status_t bmp581_read_raw(bmp581_t *dev, bmp581_raw_t *out);

/**
 * @brief Check whether a new pressure/temperature sample is ready.
 *
 * Reads INT_STATUS (reg 0x27) and reports drdy_data_reg (bit 0).
 * BST-BMP581-DS004-13 §7.21 (p.58): INT_STATUS is **clear-on-read** --
 * reading it clears drdy_data_reg *and every other asserted bit in the
 * same register* (fifo_full, fifo_ths, oor_p, por), regardless of
 * whether this call's caller cares about those sources. If the app also
 * enables FIFO or out-of-range interrupts via @ref bmp581_set_int_sources,
 * a second consumer polling those conditions on INT_STATUS will see them
 * silently cleared by this call. Don't mix this predicate with another
 * INT_STATUS reader unless that's the intended behaviour.
 *
 * @param[in]  dev       Initialised driver context.
 * @param[out] ready_out Set to true if drdy_data_reg was set (and has now
 *                        been cleared by this read), false otherwise.
 * @return ALP_OK on success, ALP_ERR_NOT_READY if @p dev isn't initialised,
 *         ALP_ERR_INVAL if @p ready_out is NULL.
 */
alp_status_t bmp581_data_ready(bmp581_t *dev, bool *ready_out);

/**
 * @brief Configure the INT pin's electrical behaviour and enable/disable it.
 *
 * Writes INT_CONFIG (reg 0x14): int_en, int_od (drive), int_pol
 * (polarity), int_mode (pulsed/latched). BST-BMP581-DS004-13 §7.5
 * (pp.52-53). This driver has no board knowledge -- the caller picks
 * @p drive / @p polarity to match whatever the INT line is wired to
 * (e.g. a GPIO expander input wants a specific polarity/drive combo;
 * this driver doesn't know or care which board that is).
 *
 * Reset default (0x35) is int_en=0 (disabled), int_od=open-drain,
 * int_pol=active-low, int_mode=latched -- call this explicitly rather
 * than relying on the reset state if the app needs the pin.
 *
 * @param[in] dev      Initialised driver context.
 * @param[in] enable   Enable (true) or disable (false) the INT pin (int_en).
 * @param[in] drive    Push-pull or open-drain output stage (int_od).
 * @param[in] polarity Active-high or active-low (int_pol).
 * @param[in] mode     Pulsed or latched signalling (int_mode).
 * @return ALP_OK on success, ALP_ERR_NOT_READY if @p dev isn't initialised.
 */
alp_status_t bmp581_configure_int_pin(bmp581_t             *dev,
                                      bool                  enable,
                                      bmp581_int_drive_t    drive,
                                      bmp581_int_polarity_t polarity,
                                      bmp581_int_mode_t     mode);

/**
 * @brief Select which conditions assert the INT pin and INT_STATUS bits.
 *
 * Writes INT_SOURCE (reg 0x15) with @p source_mask, a bitwise OR of
 * `BMP581_INT_SRC_*`. BST-BMP581-DS004-13 §7.6 (p.54). Pass 0 to
 * disable all sources except POR/soft-reset-complete, which the chip
 * always reports regardless of this register.
 *
 * @param[in] dev         Initialised driver context.
 * @param[in] source_mask Bitwise OR of `BMP581_INT_SRC_*`.
 * @return ALP_OK on success, ALP_ERR_NOT_READY if @p dev isn't initialised,
 *         ALP_ERR_INVAL if @p source_mask has bits outside the declared set.
 */
alp_status_t bmp581_set_int_sources(bmp581_t *dev, uint8_t source_mask);

/** Convert a raw reading into Pa + (°C × 1000). */
alp_status_t bmp581_compensate(const bmp581_raw_t *raw, bmp581_compensated_t *out);

/** Soft-reset the chip (writes 0xB6 to CMD register). */
alp_status_t bmp581_soft_reset(bmp581_t *dev);

/** Release the driver context. */
void bmp581_deinit(bmp581_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_BMP581_H */
