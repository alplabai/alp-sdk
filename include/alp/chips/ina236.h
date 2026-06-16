/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ina236.h
 * @brief TI INA236 16-bit, 0.1 % digital high-side current /
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *        bus-voltage / power monitor driver.
 *
 * The INA236 measures current via a sense resistor in the high
 * side of the rail under test, plus the rail's bus voltage,
 * and reports both as 16-bit values plus a calculated power
 * register.  See TI datasheet SBOSA38A (April 2024) for the
 * full register set; this driver covers the subset typical for
 * always-on rail monitoring (init, read shunt voltage, read bus
 * voltage, read current, read power, calibrate).
 *
 * Address strap (variant + A0 pin):
 *   INA236A: 0x40..0x43  (A0 = GND/VS/SDA/SCL)
 *   INA236B: 0x48..0x4B  (same A0 encoding; different upper-nibble)
 * On the E1M EVK the SDK uses six instances at 0x40, 0x41, 0x42,
 * 0x48, 0x49, 0x4A covering the +3V3, +1V8, +VIO, +V_CAM0,
 * +V_CAM1, and +5V rails respectively.  See
 * <alp/boards/alp_e1m_evk.h>'s `EVK_I2C_ADDR_INA236_*` macros and
 * the matching `EVK_INA236_SHUNT_*_OHMS` / `EVK_INA236_MAX_*_A`
 * pairs that callers feed to `ina236_init()`.
 *
 * Calibration:
 *   The current and power registers are computed from the shunt
 *   voltage and the chip's CALIBRATION register.  The driver
 *   accepts the rail's shunt resistance (Ohms) and the maximum
 *   expected current (Amps) at init time and programs the
 *   calibration register so the CURRENT register reads in
 *   microamps and the POWER register reads in microwatts (using
 *   the datasheet's CURRENT_LSB = max_current / 32768 formula).
 *
 * Register map (INA236, datasheet SBOSA38A table 7-1):
 *   0x00  Configuration   (RW)  Reset bit, mode, conv-time, averaging.
 *   0x01  Shunt voltage   (RO)  16-bit signed, LSB = 2.5 uV.
 *   0x02  Bus voltage     (RO)  16-bit signed, LSB = 1.6 mV.
 *   0x03  Power           (RO)  16-bit unsigned, LSB = 32 * CURRENT_LSB.
 *   0x04  Current         (RO)  16-bit signed, LSB = CURRENT_LSB.
 *   0x05  Calibration     (RW)  16-bit; sets current LSB scaling.
 *   0x06  Mask/Enable     (RW)  Alert config.
 *   0x07  Alert limit     (RW)  Alert threshold.
 *   0x3E  Manufacturer ID (RO)  0x5449 ('TI').
 *   0x3F  Device ID       (RO)  0xA080 (INA236).
 */

#ifndef ALP_CHIPS_INA236_H
#define ALP_CHIPS_INA236_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INA236_MFG_ID    0x5449u /**< "TI" — manufacturer ID const.   */
#define INA236_DEVICE_ID 0xA080u /**< INA236 device ID const.         */

/** ADC range setting — affects shunt voltage LSB and full-scale. */
typedef enum {
	INA236_ADCRANGE_81MV = 0, /**< Full-scale 81.92 mV, LSB = 2.5 uV.  Default. */
	INA236_ADCRANGE_20MV =
	    1, /**< Full-scale 20.48 mV, LSB = 625 nV.  Higher resolution at low currents. */
} ina236_adcrange_t;

typedef struct {
	bool       initialised;
	alp_i2c_t *bus;
	uint8_t    addr;
	/* Calibration parameters, retained for diagnostics. */
	float shunt_ohms;
	float max_current_a;
	/* Computed at init: CURRENT_LSB in amps.  Multiply the raw
     * CURRENT register by this to get amps; multiply by 1e6 first
     * for microamps. */
	float current_lsb_a;
	/* Cached config register so toggles don't read-modify-write. */
	uint16_t          cfg_cache;
	ina236_adcrange_t adcrange;
} ina236_t;

/**
 * @brief Probe the chip, verify mfg+device ID, and program the
 *        calibration register for the rail's shunt resistance and
 *        expected maximum current.
 *
 * @param[out] ctx            Driver context (output; populated on success).
 * @param[in]  bus            Open I2C bus handle the chip sits on.
 * @param[in]  addr_7bit      7-bit I2C address (0x40..0x47).  Use 0
 *                            to fall back to 0x40 (INA236A default).
 * @param[in]  shunt_ohms     Shunt resistance in Ohms, e.g.
 *                            0.010 for a 10 mOhm sense resistor.
 *                            Must be > 0.
 * @param[in]  max_current_a  Maximum expected rail current in
 *                            Amps.  Used to compute CURRENT_LSB =
 *                            max_current / 32768.  Set generously
 *                            (10x typical) to avoid clipping.
 * @param[in]  adcrange       ADC full-scale range; pick
 *                            INA236_ADCRANGE_20MV when the rail's
 *                            current rarely exceeds 25 % of max
 *                            (better resolution).
 *
 * @return ALP_OK on success, ALP_ERR_NOT_READY on probe failure
 *         (wrong device ID), ALP_ERR_INVAL on bad parameters.
 */
alp_status_t ina236_init(ina236_t         *ctx,
                         alp_i2c_t        *bus,
                         uint8_t           addr_7bit,
                         float             shunt_ohms,
                         float             max_current_a,
                         ina236_adcrange_t adcrange);

/** @brief Read the bus voltage in millivolts. */
alp_status_t ina236_read_bus_mv(ina236_t *ctx, int32_t *mv_out);

/** @brief Read the shunt voltage in microvolts.  Signed (sign
 *  follows current direction through the shunt). */
alp_status_t ina236_read_shunt_uv(ina236_t *ctx, int32_t *uv_out);

/** @brief Read current through the shunt in microamps.  Signed. */
alp_status_t ina236_read_current_ua(ina236_t *ctx, int32_t *ua_out);

/** @brief Read instantaneous power in microwatts.  Unsigned. */
alp_status_t ina236_read_power_uw(ina236_t *ctx, uint32_t *uw_out);

/**
 * @brief Convenience: read all four (bus_mv, shunt_uv, current_ua,
 *        power_uw) in one I2C transaction sequence.  Cheaper than
 *        four separate calls when polling at high rate.
 */
typedef struct {
	int32_t  bus_mv;
	int32_t  shunt_uv;
	int32_t  current_ua;
	uint32_t power_uw;
} ina236_sample_t;

/** @brief Read shunt voltage + bus voltage + current + power into one struct. */
alp_status_t ina236_read_all(ina236_t *ctx, ina236_sample_t *sample_out);

/** @brief Soft-reset the chip and rerun the calibration step.
 *  Useful after a brown-out or detected bus-voltage glitch. */
alp_status_t ina236_reset(ina236_t *ctx);

/** @brief Release the driver context.  Idempotent. */
void ina236_deinit(ina236_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_INA236_H */
