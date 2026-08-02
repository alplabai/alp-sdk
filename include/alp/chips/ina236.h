/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ina236.h
 * @brief TI INA236 16-bit, 0.1 % digital high-side current /
 *
 * @par Verification status: [SILICON-VERIFIED] on the E1M-AEN801 (Ensemble E8)
 *   bench, 2026-07-25, against the EVK's six monitors on SoC I2C2.  The
 *   scaling was cross-checked two independent ways on the +5V rail
 *   (U30 @0x4A, 20 mOhm): the POWER register reported 0.492 W where
 *   bus-voltage x shunt-derived current gave 0.5125 W (4 % agreement),
 *   which pins BOTH the eq.-4 power LSB and the ADCRANGE=1 SHUNT_CAL/4
 *   rescaling -- either one wrong would miss by 625x or 4x respectively.
 *   See docs/measuring-inference-energy.md.
 *        bus-voltage / power monitor driver.
 *
 * The INA236 measures current via a sense resistor in the high
 * side of the rail under test, plus the rail's bus voltage,
 * and reports both as 16-bit values plus a calculated power
 * register.  Every register address, bit position, LSB size and
 * equation in this driver is transcribed from TI datasheet
 * **SBOSA81D** (INA236, MAY 2021 - REVISED AUGUST 2023); the
 * section/table/equation number is cited at each use site.  This
 * driver covers the subset typical for always-on rail monitoring
 * (init, read shunt voltage, read bus voltage, read current, read
 * power, calibrate) plus the averaging / conversion-time /
 * conversion-ready controls an energy-integration sampler needs.
 *
 * Address strap (variant + A0 pin):
 *   INA236A: 0x40..0x43  (A0 = GND/VS/SDA/SCL)
 *   INA236B: 0x48..0x4B  (same A0 encoding; different upper-nibble)
 * On the E1M EVK the SDK uses six instances at 0x40, 0x41, 0x42,
 * 0x4B, 0x49, 0x4A covering the +3V3, +1V8, +VIO, +V_CAM0,
 * +V_CAM1, and +5V rails respectively -- the list is rail-ordered,
 * not address-ordered, so +V_CAM0 reads 0x4B (re-strapped A0=SCL
 * from the next batch; PRE-RESPIN boards had it at 0x48).  See
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
 * Register map (INA236, datasheet SBOSA81D section 7.6.1):
 *   0x00  Configuration   (RW)  Reset bit, ADCRANGE, averaging, conv-time, mode.
 *   0x01  Shunt voltage   (RO)  16-bit signed; LSB = 2.5 uV (ADCRANGE=0)
 *                               or 625 nV (ADCRANGE=1).
 *   0x02  Bus voltage     (RO)  15-bit positive-only, LSB = 1.6 mV.
 *   0x03  Power           (RO)  16-bit unsigned, LSB = 32 * CURRENT_LSB (eq. 4).
 *   0x04  Current         (RO)  16-bit signed, LSB = CURRENT_LSB (eq. 3).
 *   0x05  Calibration     (RW)  15-bit SHUNT_CAL; sets current LSB scaling (eq. 1).
 *   0x06  Mask/Enable     (RW)  Alert config + the CVRF conversion-ready flag.
 *   0x07  Alert limit     (RW)  Alert threshold.
 *   0x3E  Manufacturer ID (RO)  0x5449 ('TI') -- the reliable identity gate.
 *   0x3F  Device ID       (RO)  DIEID[15:4]+REV[3:0]; varies by lot (0xA480 read
 *                               on the E1M-AEN801 bench), so NOT an identity gate.
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

/** ADC range setting — affects shunt voltage LSB and full-scale.
 *  CONFIG bit 12 (SBOSA81D table 7-4).  Note this also rescales
 *  SHUNT_CAL: the driver divides the eq.-1 value by 4 for
 *  INA236_ADCRANGE_20MV, exactly as SBOSA81D section 8.1.2 requires. */
typedef enum {
	INA236_ADCRANGE_81MV = 0, /**< Full-scale ±81.92 mV, LSB = 2.5 uV.  Default. */
	INA236_ADCRANGE_20MV =
	    1, /**< Full-scale ±20.48 mV, LSB = 625 nV.  Higher resolution at low currents. */
} ina236_adcrange_t;

/** CONFIG AVG field (bits 11-9): ADC conversions averaged before the
 *  read-back registers update.  Codes per SBOSA81D table 7-4. */
typedef enum {
	INA236_AVG_1    = 0, /**< 000b = 1 (reset default). */
	INA236_AVG_4    = 1, /**< 001b = 4.    */
	INA236_AVG_16   = 2, /**< 010b = 16.   */
	INA236_AVG_64   = 3, /**< 011b = 64.   */
	INA236_AVG_128  = 4, /**< 100b = 128.  */
	INA236_AVG_256  = 5, /**< 101b = 256.  */
	INA236_AVG_512  = 6, /**< 110b = 512.  */
	INA236_AVG_1024 = 7, /**< 111b = 1024. */
} ina236_avg_t;

/** CONFIG VBUSCT (bits 8-6) / VSHCT (bits 5-3) conversion-time codes.
 *  Both fields share this encoding; times per SBOSA81D table 7-4. */
typedef enum {
	INA236_CT_140US  = 0, /**< 000b = 140 us.  */
	INA236_CT_204US  = 1, /**< 001b = 204 us.  */
	INA236_CT_332US  = 2, /**< 010b = 332 us.  */
	INA236_CT_588US  = 3, /**< 011b = 588 us.  */
	INA236_CT_1100US = 4, /**< 100b = 1100 us (reset default). */
	INA236_CT_2116US = 5, /**< 101b = 2116 us. */
	INA236_CT_4156US = 6, /**< 110b = 4156 us. */
	INA236_CT_8244US = 7, /**< 111b = 8244 us. */
} ina236_ct_t;

/** CONFIG MODE field (bits 2-0), per SBOSA81D table 7-4.  Note the
 *  datasheet assigns Shutdown to BOTH 000b and 100b. */
typedef enum {
	INA236_MODE_SHUTDOWN       = 0, /**< 000b = Shutdown. */
	INA236_MODE_SHUNT_TRIG     = 1, /**< 001b = Shunt voltage, triggered single-shot. */
	INA236_MODE_BUS_TRIG       = 2, /**< 010b = Bus voltage, triggered single-shot. */
	INA236_MODE_SHUNT_BUS_TRIG = 3, /**< 011b = Shunt + bus, triggered single-shot. */
	INA236_MODE_SHUTDOWN_ALT   = 4, /**< 100b = Shutdown (second encoding). */
	INA236_MODE_SHUNT_CONT     = 5, /**< 101b = Continuous shunt voltage. */
	INA236_MODE_BUS_CONT       = 6, /**< 110b = Continuous bus voltage. */
	INA236_MODE_SHUNT_BUS_CONT = 7, /**< 111b = Continuous shunt + bus (reset default). */
} ina236_mode_t;

typedef struct {
	bool       initialised;
	alp_i2c_t *bus;
	uint8_t    addr;
	/* Calibration parameters, retained for diagnostics. */
	float shunt_ohms;
	float max_current_a;
	/* Computed at init: CURRENT_LSB in amps.  Multiply the raw
     * CURRENT register by this to get amps; multiply by 1e6 first
     * for microamps.  Derived from min(max_current_a, full_scale_a) --
     * see full_scale_a. */
	float current_lsb_a;
	/* Computed at init: the largest current this ADCRANGE + shunt can
     * measure at all, = (ADCRANGE full-scale shunt voltage) / shunt_ohms.
     * Currents above it saturate the shunt ADC regardless of
     * max_current_a, so the reporting scale is never derived from a
     * value wider than this.  Read it to know the real measurement
     * ceiling: for a 20 mOhm shunt it is 4.096 A on the +/-81.92 mV
     * range and 1.024 A on the +/-20.48 mV range. */
	float full_scale_a;
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
 * @param[in]  addr_7bit      7-bit I2C address: 0x40..0x43 (INA236A)
 *                            or 0x48..0x4B (INA236B), per the strap
 *                            table above.  Use 0 to fall back to
 *                            0x40 (INA236A default).  Any other value
 *                            (including the 0x80..0xFF band an
 *                            8-bit-encoded address would land in)
 *                            returns ALP_ERR_INVAL.
 * @param[in]  shunt_ohms     Shunt resistance in Ohms, e.g.
 *                            0.010 for a 10 mOhm sense resistor.
 *                            Must be finite and > 0.
 * @param[in]  max_current_a  Maximum expected rail current in Amps —
 *                            the REPORTING scale, not a hardware limit.
 *                            CURRENT_LSB = min(max_current_a,
 *                            `full_scale_a`) / 32768, so asking for more
 *                            than the ADCRANGE can measure gains nothing
 *                            and is clamped; asking for LESS is a
 *                            deliberate resolution-for-headroom trade and
 *                            is honoured (currents above it saturate the
 *                            CURRENT/POWER registers).  Must be finite
 *                            and > 0.  Do NOT "set generously to avoid
 *                            clipping": clipping is set by `adcrange` and
 *                            the shunt, and a needlessly wide value only
 *                            coarsens every reading.
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

/**
 * @brief Program the CONFIG averaging, conversion-time and operating-mode
 *        fields (SBOSA81D table 7-4), preserving RST and ADCRANGE.
 *
 * `ina236_init()` leaves these at the reset defaults (AVG=1,
 * VBUSCT=VSHCT=1100 us, continuous shunt+bus).  A sampler that has to
 * hit a target sample rate — e.g. energy integration over a short
 * active window — sets them explicitly instead: the conversion period
 * is AVG x (VBUSCT + VSHCT) in continuous shunt+bus mode, so it trades
 * noise against rate.  Use ina236_sample_period_us() to compute the
 * resulting period before committing to a window length.
 *
 * @param[in,out] ctx     Initialised driver context.
 * @param[in]     avg     Averaging code.
 * @param[in]     vbusct  Bus-voltage conversion time.
 * @param[in]     vshct   Shunt-voltage conversion time.
 * @param[in]     mode    Operating mode.
 *
 * @return ALP_OK on success, ALP_ERR_NOT_READY if ctx is null/uninitialised,
 *         ALP_ERR_INVAL if any field is out of range.
 */
alp_status_t ina236_configure(ina236_t     *ctx,
                              ina236_avg_t  avg,
                              ina236_ct_t   vbusct,
                              ina236_ct_t   vshct,
                              ina236_mode_t mode);

/**
 * @brief Number of conversions an AVG code averages (SBOSA81D table 7-4):
 *        1, 4, 16, 64, 128, 256, 512, 1024.  0 for an out-of-range code.
 *
 * Exposed so a caller reporting its own configuration reads the datasheet
 * table from the driver that owns it, instead of restating a literal that
 * silently stops matching the code actually programmed.
 */
uint16_t ina236_avg_count(ina236_avg_t avg);

/**
 * @brief Conversion time in microseconds for a VBUSCT/VSHCT code (SBOSA81D
 *        table 7-4): 140, 204, 332, 588, 1100, 2116, 4156, 8244.  0 for an
 *        out-of-range code.  Both fields share the encoding.
 */
uint16_t ina236_conversion_time_us(ina236_ct_t ct);

/**
 * @brief Conversion period in microseconds for a CONFIG field combination.
 *
 * AVG multiplies the summed conversion time of whichever measurements
 * the mode actually performs (SBOSA81D section 7.4.4): shunt+bus adds
 * both conversion times, the single-quantity modes only their own.
 * Returns 0 for a shutdown mode (no conversions happen), which also
 * makes "divide by the period" call sites fail loudly rather than
 * silently reporting an impossible rate.
 *
 * Pure function of the datasheet tables — takes no context, so a caller
 * can size its window before touching the bus.
 */
uint32_t ina236_sample_period_us(ina236_avg_t  avg,
                                 ina236_ct_t   vbusct,
                                 ina236_ct_t   vshct,
                                 ina236_mode_t mode);

/**
 * @brief Poll the conversion-ready flag (CVRF, Mask/Enable bit 3).
 *
 * CVRF is set after all conversions, averaging and multiplications
 * complete, and — per SBOSA81D table 7-10 — is cleared by *reading the
 * Mask/Enable register*.  This call performs that read, so it is
 * self-clearing: a caller that reads a measurement register only when
 * this returns `true` consumes each conversion exactly once instead of
 * blind-rate polling (which double-counts or misses samples).
 *
 * CVRF is set regardless of the CNVR enable bit (bit 10); CNVR only
 * routes the flag to the external ALERT pin, which this polling path
 * does not use, so the driver leaves Mask/Enable's alert bits alone.
 *
 * @param[in,out] ctx        Initialised driver context.
 * @param[out]    ready_out  true if a fresh conversion was pending.
 *
 * @return ALP_OK on success, ALP_ERR_NOT_READY if ctx is null/uninitialised
 *         or ready_out is null.
 */
alp_status_t ina236_conversion_ready(ina236_t *ctx, bool *ready_out);

/**
 * @brief Read the raw 16-bit POWER register (0x03), unscaled.
 *
 * For high-rate energy integration: streaming the raw counts plus one
 * ina236_power_lsb_w() scale factor avoids re-quantising every sample
 * through the integer microwatt conversion in ina236_read_power_uw(),
 * whose LSB is coarse on a high-current rail (32 x CURRENT_LSB).
 */
alp_status_t ina236_read_power_raw(ina236_t *ctx, uint16_t *raw_out);

/**
 * @brief Watts per POWER-register count for this context's calibration:
 *        32 x CURRENT_LSB (SBOSA81D eq. 4).  0.0f if ctx is null.
 */
float ina236_power_lsb_w(const ina236_t *ctx);

/**
 * @brief Largest current this shunt + ADC range can measure at all:
 *        (ADCRANGE full-scale shunt voltage) / shunt_ohms.
 *
 * Currents above it saturate the shunt ADC no matter what scale the caller
 * asked for, so this is the real measurement ceiling. 20 mOhm gives 4.096 A
 * on the +/-81.92 mV range and 1.024 A on the +/-20.48 mV range. Pure
 * function of the datasheet full scales; returns 0.0f for a non-positive or
 * non-finite shunt.
 */
float ina236_full_scale_a(float shunt_ohms, ina236_adcrange_t adcrange);

/**
 * @brief Compute the SHUNT_CAL register value and the CURRENT_LSB it yields,
 *        without touching the bus.
 *
 * This is the calibration arithmetic `ina236_init()` itself uses (SBOSA81D
 * eq. 1 plus the §8.1.2 ADCRANGE=1 divide-by-4), exposed because it is the
 * part worth testing and the part that has been wrong: it is a pure function,
 * so a unit test drives the same code that ships instead of restating it.
 *
 * `current_lsb_a_out` is derived from the SHUNT_CAL actually produced, not
 * from `max_current_a` — the register is a 15-bit integer that both rounds and
 * saturates, and carrying the requested LSB past either would leave every
 * current and power reading wrong by the discarded ratio.
 *
 * @param[in]  shunt_ohms     Shunt resistance in Ohms; finite and > 0.
 * @param[in]  max_current_a  Requested reporting scale in Amps; clamped to
 *                            ina236_full_scale_a() (see ina236_init()).
 * @param[in]  adcrange       ADC full-scale range.
 * @param[out] shunt_cal_out  SHUNT_CAL to program (CALIBRATION bits 14-0).
 * @param[out] current_lsb_a_out  Amps per CURRENT-register count.
 *
 * @return ALP_OK, or ALP_ERR_INVAL on a null out-pointer, a non-finite or
 *         non-positive input, or a request whose SHUNT_CAL rounds to 0 (which
 *         the datasheet defines as "report zero current and power", so it
 *         cannot be calibrated rather than silently always reading zero).
 */
alp_status_t ina236_calibration_for(float             shunt_ohms,
                                    float             max_current_a,
                                    ina236_adcrange_t adcrange,
                                    uint16_t         *shunt_cal_out,
                                    float            *current_lsb_a_out);

/** @brief Soft-reset the chip and rerun the calibration step.
 *  Useful after a brown-out or detected bus-voltage glitch. */
alp_status_t ina236_reset(ina236_t *ctx);

/** @brief Release the driver context.  Idempotent. */
void ina236_deinit(ina236_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_INA236_H */
