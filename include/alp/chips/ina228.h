/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ina228.h
 * @brief TI INA228 85 V, 20-bit power / energy / charge monitor driver.
 *
 * The INA228 measures the voltage across a sense resistor (current), the bus
 * voltage and its own die temperature, and integrates power, energy and
 * charge on chip.  Every register address, width, LSB size and equation in
 * this driver is transcribed from TI datasheet SLYS021A (INA228, January
 * 2021, revised May 2022); the table / equation number is cited at each use.
 *
 * @par Driver status: [partial-impl] init / identity probe, ADC_CONFIG,
 *   SHUNT_CAL, shunt / bus / die-temperature / current / power / energy /
 *   charge reads, accumulator reset, the DIAG_ALRT read, the selectable shunt
 *   range (ina228_init(), ina228_set_shunt_range(), ina228_pick_adcrange())
 *   and over-range detection (ina228_check_over_range()).  The alert
 *   threshold registers (SOVL, SUVL, BOVL, BUVL, TEMP_LIMIT, PWR_LIMIT) and
 *   SHUNT_TEMPCO are not exposed.
 *
 * @par Verification status: [UNVERIFIED ON SILICON].  The driver has not
 *   been run on a real INA228.  The only bench fact on record is that
 *   address 0x42 acknowledged on an E1M-X EVK on 2026-10-02, with no ID
 *   register read.
 *
 * @par ADR 0017 position:
 *   Upstream Zephyr ships a `ti,ina228` driver (drivers/sensor/ti/ina2xx) and
 *   that is the path for a bus a Zephyr core masters.  This portable driver
 *   exists for the case the upstream one cannot reach: the E1M-X EVK's INA228
 *   is on E1M-X I2C0, which Linux owns on the A55 (i2c-0) and the Linux
 *   kernel in use has no INA228 support, so the SDK reads it from user space
 *   over i2c-dev, the same way `ina236.h` is used there.
 *
 * @par Hardware caveat (E1M-X EVK):
 *   On the current E1M-X EVK revision the device's I2C bus pins are
 *   documented as swapped and are corrected by a hand rework.  The device
 *   answers at 0x42 only on carriers with that rework; a carrier without it
 *   does not answer.  ina228_init() reports that as ::INA228_ERR_NOT_PRESENT,
 *   which a caller should treat as "part absent", not as a fault.
 *
 * @par Units:
 *   Integer math only.  Resistance in micro-ohms, current in micro-amps,
 *   voltage in micro-volts, power in micro-watts, energy in micro-joules,
 *   charge in micro-coulombs, temperature in milli-degrees Celsius.  The
 *   40-bit ENERGY / CHARGE registers are held in 64-bit values.
 *
 * Register map (SLYS021A table 7-3):
 *   0x00 CONFIG        RST, RSTACC, CONVDLY, TEMPCOMP, ADCRANGE.
 *   0x01 ADC_CONFIG    MODE, VBUSCT, VSHCT, VTCT, AVG.
 *   0x02 SHUNT_CAL     15-bit current scaling constant (eq. 2).
 *   0x04 VSHUNT        24-bit, signed 20-bit value in bits 23:4.
 *   0x05 VBUS          24-bit, 20-bit value in bits 23:4, 195.3125 uV/LSB.
 *   0x06 DIETEMP       16-bit signed, 7.8125 m degC/LSB.
 *   0x07 CURRENT       24-bit, signed 20-bit value in bits 23:4.
 *   0x08 POWER         24-bit unsigned.
 *   0x09 ENERGY        40-bit unsigned.
 *   0x0A CHARGE        40-bit signed.
 *   0x0B DIAG_ALRT     Diagnostic flags and alert configuration.
 *   0x3E MANUFACTURER_ID  0x5449 ("TI").
 *   0x3F DEVICE_ID     DIEID[15:4] = 0x228, REV[3:0].
 *
 * Address strap: A1 and A0 each go to GND, VS, SDA or SCL, giving 16
 * addresses, 0x40..0x4F.  Every register is big-endian on the wire.
 */

#ifndef ALP_CHIPS_INA228_H
#define ALP_CHIPS_INA228_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INA228_MFG_ID 0x5449u /**< MANUFACTURER_ID (0x3E) value: "TI" in ASCII. */
#define INA228_DIE_ID 0x228u  /**< DEVICE_ID (0x3F) bits 15:4, the die identifier. */

/**
 * @brief Status ina228_init() returns when nothing acknowledges at the
 *        address, i.e. the part is not present (or not reachable).
 *
 * Returned for a no-ACK from the bus, or a bus error indistinguishable from
 * one (the backend reports ::ALP_ERR_IO on Zephyr, and ::ALP_ERR_IO or
 * ::ALP_ERR_NOT_READY, from ENXIO, on Linux i2c-dev).  Any other bus failure
 * (::ALP_ERR_TIMEOUT, ::ALP_ERR_NOSUPPORT, ...) is returned unchanged, so a
 * distinguishable fault is not reported as "absent".  Distinct from ::ALP_ERR_NOT_READY, which
 * ina228_init() returns when something answered but is not an INA228.  On a
 * carrier without the bus-pin rework this is the normal result.
 *
 * alp_status_t has no dedicated "no such device" code.  ::ALP_ERR_NOT_FOUND
 * ("the requested item is absent") is the closest, and no other peripheral
 * uses it for a bus probe, so a caller can test for it unambiguously.
 */
#define INA228_ERR_NOT_PRESENT ALP_ERR_NOT_FOUND

/** First and last strap-selectable 7-bit address. */
#define INA228_ADDR_MIN 0x40u /**< Lowest 7-bit address (A1 = A0 = GND). */
#define INA228_ADDR_MAX 0x4Fu /**< Highest 7-bit address (A1 = A0 = SCL). */

/** Largest max-current request, in micro-amps, ina228_init() accepts. */
#define INA228_MAX_CURRENT_LIMIT_UA 1000000000u

/** DIAG_ALRT (0x0B) flag bits, SLYS021A table 7-16. */
#define INA228_DIAG_ENERGYOF (1u << 11) /**< ENERGY register overflowed; clears on ENERGY read. */
#define INA228_DIAG_CHARGEOF (1u << 10) /**< CHARGE register overflowed; clears on CHARGE read. */
#define INA228_DIAG_MATHOF   (1u << 9)  /**< Arithmetic overflow: current / power may be invalid. */
#define INA228_DIAG_TMPOL    (1u << 7)  /**< Temperature over-limit event. */
#define INA228_DIAG_SHNTOL   (1u << 6)  /**< Shunt over-voltage event. */
#define INA228_DIAG_SHNTUL   (1u << 5)  /**< Shunt under-voltage event. */
#define INA228_DIAG_BUSOL    (1u << 4)  /**< Bus over-voltage event. */
#define INA228_DIAG_BUSUL    (1u << 3)  /**< Bus under-voltage event. */
#define INA228_DIAG_POL      (1u << 2)  /**< Power over-limit event. */
#define INA228_DIAG_CNVRF    (1u << 1)  /**< Conversion complete. */
#define INA228_DIAG_MEMSTAT  (1u << 0)  /**< 1 = normal; 0 = trim-memory checksum error. */

/** Shunt full-scale range, CONFIG bit 4 (SLYS021A table 7-5).  Also scales
 *  SHUNT_CAL: the driver multiplies the eq.-2 value by 4 for
 *  ::INA228_ADCRANGE_40MV. */
typedef enum {
	INA228_ADCRANGE_163MV = 0, /**< +/-163.84 mV full scale, 312.5 nV/LSB.  Reset default. */
	INA228_ADCRANGE_40MV  = 1, /**< +/-40.96 mV full scale, 78.125 nV/LSB. */
} ina228_adcrange_t;

/** ADC_CONFIG AVG field (bits 2:0): conversions averaged per result
 *  (SLYS021A table 7-6). */
typedef enum {
	INA228_AVG_1    = 0, /**< 1 sample (reset default). */
	INA228_AVG_4    = 1, /**< 4 samples. */
	INA228_AVG_16   = 2, /**< 16 samples. */
	INA228_AVG_64   = 3, /**< 64 samples. */
	INA228_AVG_128  = 4, /**< 128 samples. */
	INA228_AVG_256  = 5, /**< 256 samples. */
	INA228_AVG_512  = 6, /**< 512 samples. */
	INA228_AVG_1024 = 7, /**< 1024 samples. */
} ina228_avg_t;

/** ADC_CONFIG VBUSCT / VSHCT / VTCT conversion-time code (3 bits each,
 *  shared encoding, SLYS021A table 7-6). */
typedef enum {
	INA228_CT_50US   = 0, /**< 50 us. */
	INA228_CT_84US   = 1, /**< 84 us. */
	INA228_CT_150US  = 2, /**< 150 us. */
	INA228_CT_280US  = 3, /**< 280 us. */
	INA228_CT_540US  = 4, /**< 540 us. */
	INA228_CT_1052US = 5, /**< 1052 us (reset default). */
	INA228_CT_2074US = 6, /**< 2074 us. */
	INA228_CT_4120US = 7, /**< 4120 us. */
} ina228_ct_t;

/** ADC_CONFIG MODE field (bits 15:12), SLYS021A table 7-6.  Bit 3 selects
 *  continuous over triggered; 0h and 8h are both Shutdown. */
typedef enum {
	INA228_MODE_SHUTDOWN       = 0x0, /**< Shutdown. */
	INA228_MODE_BUS_TRIG       = 0x1, /**< Triggered bus voltage, single shot. */
	INA228_MODE_SHUNT_TRIG     = 0x2, /**< Triggered shunt voltage, single shot. */
	INA228_MODE_SHUNT_BUS_TRIG = 0x3, /**< Triggered shunt + bus, single shot. */
	INA228_MODE_TEMP_TRIG      = 0x4, /**< Triggered temperature, single shot. */
	INA228_MODE_ALL_TRIG       = 0x7, /**< Triggered bus + shunt + temperature, single shot. */
	INA228_MODE_SHUTDOWN_ALT   = 0x8, /**< Shutdown (second encoding). */
	INA228_MODE_BUS_CONT       = 0x9, /**< Continuous bus voltage. */
	INA228_MODE_SHUNT_CONT     = 0xA, /**< Continuous shunt voltage. */
	INA228_MODE_SHUNT_BUS_CONT = 0xB, /**< Continuous shunt + bus. */
	INA228_MODE_TEMP_CONT      = 0xC, /**< Continuous temperature. */
	INA228_MODE_ALL_CONT       = 0xF, /**< Continuous bus + shunt + temperature (reset default). */
} ina228_mode_t;

/**
 * A VSHUNT count this close to either end of its 20-bit range is treated as
 * a clipped (over-range) reading by ina228_check_over_range().  The value 8 is
 * a driver choice, not a datasheet figure.
 */
#define INA228_SATURATION_COUNTS 8

/**
 * Driver context.  Treat as opaque; fields are exposed for diagnostics.
 *
 * Thread safety: none inside the driver.  One caller at a time per device
 * (ina228_read_*() and the write calls are separate bus transactions and the
 * context caches CONFIG), while the underlying bus handle may be shared
 * between several devices, each with its own context, as the bus layer
 * serialises transfers.
 */
typedef struct {
	bool       initialised;      /**< True once ina228_init() succeeded. */
	alp_i2c_t *bus;              /**< Open I2C bus handle. */
	uint8_t    addr;             /**< 7-bit I2C address. */
	uint32_t   shunt_micro_ohms; /**< Shunt resistance as given to ina228_init(). */
	uint32_t
	    max_current_ua; /**< Reporting scale: ina228_init(), or the last ina228_set_shunt_range(). */
	uint16_t          shunt_cal;      /**< SHUNT_CAL value programmed. */
	uint64_t          current_lsb_pa; /**< CURRENT_LSB in pico-amps, derived from shunt_cal. */
	uint16_t          config_cache;   /**< Last CONFIG value written (RST / RSTACC clear). */
	ina228_adcrange_t adcrange;       /**< Current ADC range (init, or ina228_set_shunt_range()). */
} ina228_t;

/**
 * @brief Largest current the shunt and ADC range can measure at all, in
 *        micro-amps: (full-scale shunt voltage) / R_SHUNT.
 *
 * Pure function.  Returns 0 for a zero shunt.
 *
 * @param[in] shunt_micro_ohms  Shunt resistance in micro-ohms.
 * @param[in] adcrange          ADC range.
 * @return Full-scale current in micro-amps (at most 163840 * 1e6 uA).
 */
uint64_t ina228_full_scale_ua(uint32_t shunt_micro_ohms, ina228_adcrange_t adcrange);

/**
 * @brief Pick the shunt range for a shunt and an expected maximum current.
 *
 * Chooses ::INA228_ADCRANGE_40MV (finer: four times the resolution) when
 * max current x R_SHUNT fits +/-40.96 mV, otherwise ::INA228_ADCRANGE_163MV
 * when it fits +/-163.84 mV.  For the E1M-X EVK's 100 mOhm shunt that is
 * up to 0.4096 A on the narrow range and up to 1.6384 A on the wide one.
 * Pure function; the boundary values are inclusive (0.4096 A -> narrow).
 *
 * @param[in]  shunt_micro_ohms  Shunt resistance, micro-ohms, > 0.
 * @param[in]  max_current_ua    Expected maximum current, micro-amps, > 0.
 * @param[out] range_out         The selected range.
 *
 * @return ALP_OK; ALP_ERR_INVAL for a NULL output or a zero shunt / current;
 *         ALP_ERR_OUT_OF_RANGE when the current fits neither range.
 */
alp_status_t ina228_pick_adcrange(uint32_t           shunt_micro_ohms,
                                  uint32_t           max_current_ua,
                                  ina228_adcrange_t *range_out);

/**
 * @brief Compute SHUNT_CAL and the CURRENT_LSB it yields, without touching
 *        the bus.
 *
 * SHUNT_CAL = 13107.2e6 * CURRENT_LSB * R_SHUNT (SLYS021A eq. 2), times 4
 * for ::INA228_ADCRANGE_40MV, with CURRENT_LSB = max current / 2^19 (eq. 3).
 * A @p max_current_ua above ina228_full_scale_ua() for the range is rejected.
 * This differs from ina236_calibration_for(), which clamps the request to the
 * range's full scale: clamping would silently change the scale the caller
 * asked for, so here an out-of-range request is an error and any request up
 * to the full scale is honoured.  Pass ina228_full_scale_ua() for the finest
 * resolution the shunt allows.  (Within the full scale SHUNT_CAL is at most
 * about 4096, i.e. 4096 x max current / range full scale, far below the 0x7FFF
 * register ceiling, which the function still checks.)  The returned LSB is back-computed from the
 * rounded SHUNT_CAL, so it matches what the register actually does.
 *
 * @param[in]  shunt_micro_ohms  Shunt resistance, micro-ohms, > 0.
 * @param[in]  max_current_ua    Requested reporting scale, micro-amps,
 *                               1..::INA228_MAX_CURRENT_LIMIT_UA.
 * @param[in]  adcrange          ADC range.
 * @param[out] shunt_cal_out     SHUNT_CAL to program (15 bits).
 * @param[out] lsb_pa_out        CURRENT_LSB in pico-amps.
 *
 * @return ALP_OK; ALP_ERR_INVAL for a NULL output, a zero shunt or a zero /
 *         over-limit current; ALP_ERR_OUT_OF_RANGE when @p max_current_ua is
 *         above the range's full scale, or SHUNT_CAL would round to 0 or
 *         exceed 0x7FFF (the pair cannot be encoded).
 */
alp_status_t ina228_calibration_for(uint32_t          shunt_micro_ohms,
                                    uint32_t          max_current_ua,
                                    ina228_adcrange_t adcrange,
                                    uint16_t         *shunt_cal_out,
                                    uint64_t         *lsb_pa_out);

/**
 * @brief Probe the part, check its identity and program SHUNT_CAL.
 *
 * Reads MANUFACTURER_ID (must be ::INA228_MFG_ID) and DEVICE_ID (bits 15:4
 * must be ::INA228_DIE_ID), selects the ADC range, then writes SHUNT_CAL.
 * ADC_CONFIG is left at its reset default (continuous bus + shunt +
 * temperature, 1052 us each, no averaging); call ina228_configure() to change
 * it.
 *
 * @param[out] ctx               Driver context, populated on success.
 * @param[in]  bus               Open I2C bus handle.
 * @param[in]  addr_7bit         7-bit address, ::INA228_ADDR_MIN..::INA228_ADDR_MAX.
 * @param[in]  shunt_micro_ohms  Shunt resistance in micro-ohms, e.g. 100000
 *                               for 100 milli-ohms.  > 0.
 * @param[in]  max_current_ua    Reporting scale in micro-amps, at most the
 *                               range's full scale (see
 *                               ina228_calibration_for()).  CURRENT_LSB is this
 *                               value / 2^19, so a current above it overflows
 *                               the 20-bit CURRENT register (and POWER) even
 *                               when the shunt range is not exceeded; the
 *                               shunt range itself is set by @p adcrange.
 *                               Use ina228_pick_adcrange() to choose the
 *                               range for it.
 * @param[in]  adcrange          ADC range.
 *
 * @return ALP_OK on success; ::INA228_ERR_NOT_PRESENT if the first register
 *         read gets no ACK (part absent / unreachable); ALP_ERR_NOT_READY
 *         if something answered but is not an INA228; ALP_ERR_INVAL for bad
 *         parameters; ALP_ERR_OUT_OF_RANGE when SHUNT_CAL cannot be encoded;
 *         any other bus error (ALP_ERR_TIMEOUT, ...) unchanged.  On Linux
 *         ALP_ERR_BUSY (a kernel driver bound at the address) comes from the
 *         first write, whose slave-address ioctl fails, not from the first
 *         read, which is an I2C_RDWR transfer.
 */
alp_status_t ina228_init(ina228_t         *ctx,
                         alp_i2c_t        *bus,
                         uint8_t           addr_7bit,
                         uint32_t          shunt_micro_ohms,
                         uint32_t          max_current_ua,
                         ina228_adcrange_t adcrange);

/**
 * @brief Change the shunt range at run time and recompute SHUNT_CAL.
 *
 * Rewrites SHUNT_CAL (times 4 on the 40.96 mV range) and CONFIG.ADCRANGE so
 * current, power, energy and charge stay correctly scaled after the switch,
 * and clears the ENERGY and CHARGE accumulators: they were accumulated at the
 * old scale, and SLYS021A does not say the device carries them across a range
 * change.  The call validates everything before its first bus write, then
 * does three writes in this order: SHUNT_CAL (new), CONFIG (new range |
 * RSTACC), CONFIG (new range, RSTACC clear).  The datasheet says only RST
 * self-clears and is silent on RSTACC, so the accumulators are cleared by the
 * second write, at the instant range and calibration are both new, under
 * either RSTACC behaviour, and the third write leaves the bit clear.  Between
 * the first and second write the current and power registers are scaled with
 * the new calibration and the OLD range (4x off) for at most one conversion:
 * callers should discard the first sample after a switch.  On a bus error
 * part-way the device may hold the new SHUNT_CAL and/or range while the
 * context still describes the old setup, and the context is left as it was;
 * call ina228_reset() (which re-applies the context's range and calibration)
 * or ina228_init() again to make them agree.
 *
 * With the E1M-X EVK's 100 mOhm shunt the two scales are: ::INA228_ADCRANGE_163MV,
 * 1.6384 A full scale, 3.125 uA/LSB; ::INA228_ADCRANGE_40MV, 0.4096 A full
 * scale, 0.78125 uA/LSB (four times the resolution).
 *
 * @param[in,out] ctx             Initialised context.
 * @param[in]     adcrange        New range.
 * @param[in]     max_current_ua  Reporting scale for the new range, micro-amps;
 *                                0 = the new range's full scale.  Above the
 *                                full scale is rejected (see
 *                                ina228_calibration_for()).
 *
 * @return ALP_OK; ALP_ERR_NOT_READY for a NULL / uninitialised @p ctx;
 *         ALP_ERR_INVAL for a bad range; ALP_ERR_OUT_OF_RANGE when the scale
 *         does not fit the range; a bus error unchanged.
 */
alp_status_t
ina228_set_shunt_range(ina228_t *ctx, ina228_adcrange_t adcrange, uint32_t max_current_ua);

/**
 * @brief Report whether the last shunt reading is clipped (over range).
 *
 * True when DIAG_ALRT.MATHOF is set (an arithmetic overflow: current and
 * power may be invalid) or VSHUNT sits within ::INA228_SATURATION_COUNTS of
 * either end of its 20-bit range.  The saturation test is inferred from
 * SLYS021A section 7.3.5 (the detected shunt voltage stays constant once it
 * exceeds the selected ADC range), not verified on silicon, and the margin is
 * a driver choice.  A reader on the 40.96 mV range should check this
 * and not print the numbers as valid when it is true.  MATHOF is a flag only
 * cleared by another conversion or an accumulator reset; the saturation test
 * is on the live VSHUNT value.
 *
 * @param[in]  ctx       Initialised context.
 * @param[out] over_out  True when the reading is clipped.
 * @return ALP_OK; ALP_ERR_NOT_READY for a NULL / uninitialised @p ctx or NULL
 *         output; a bus error unchanged.
 */
alp_status_t ina228_check_over_range(ina228_t *ctx, bool *over_out);

/**
 * @brief Program ADC_CONFIG: operating mode, conversion times, averaging.
 *
 * @param[in,out] ctx     Initialised context.
 * @param[in]     avg     Averaging count.
 * @param[in]     vbusct  Bus-voltage conversion time.
 * @param[in]     vshct   Shunt-voltage conversion time.
 * @param[in]     vtct    Temperature conversion time.
 * @param[in]     mode    Operating mode.
 *
 * @return ALP_OK; ALP_ERR_NOT_READY if @p ctx is NULL or uninitialised;
 *         ALP_ERR_INVAL if a field is out of range.
 */
alp_status_t ina228_configure(ina228_t     *ctx,
                              ina228_avg_t  avg,
                              ina228_ct_t   vbusct,
                              ina228_ct_t   vshct,
                              ina228_ct_t   vtct,
                              ina228_mode_t mode);

/**
 * @brief Read the shunt voltage in micro-volts.  Signed.
 *
 * 312.5 nV/LSB (::INA228_ADCRANGE_163MV) or 78.125 nV/LSB
 * (::INA228_ADCRANGE_40MV), rounded to the nearest micro-volt.
 *
 * @param[in]  ctx     Initialised context.
 * @param[out] uv_out  Shunt voltage, micro-volts.
 * @return ALP_OK; ALP_ERR_NOT_READY for a NULL / uninitialised @p ctx or NULL output;
 *         a bus error from the transfer is returned unchanged.
 */
alp_status_t ina228_read_shunt_uv(ina228_t *ctx, int32_t *uv_out);

/**
 * @brief Read the bus voltage in micro-volts (195.3125 uV/LSB, rounded).
 *
 * @param[in]  ctx     Initialised context.
 * @param[out] uv_out  Bus voltage, micro-volts.
 * @return ALP_OK; ALP_ERR_NOT_READY for a NULL / uninitialised @p ctx or NULL output;
 *         a bus error from the transfer is returned unchanged.
 */
alp_status_t ina228_read_bus_uv(ina228_t *ctx, int32_t *uv_out);

/**
 * @brief Read the die temperature in milli-degrees Celsius (7.8125 m degC/LSB).
 *
 * @param[in]  ctx        Initialised context.
 * @param[out] mdegc_out  Die temperature, milli-degrees Celsius.
 * @return ALP_OK; ALP_ERR_NOT_READY for a NULL / uninitialised @p ctx or NULL output;
 *         a bus error from the transfer is returned unchanged.
 */
alp_status_t ina228_read_temp_mdegc(ina228_t *ctx, int32_t *mdegc_out);

/**
 * @brief Read the current in micro-amps.  Signed (SLYS021A eq. 4).
 *
 * @param[in]  ctx     Initialised context.
 * @param[out] ua_out  Current, micro-amps.
 * @return ALP_OK; ALP_ERR_NOT_READY for a NULL / uninitialised @p ctx or NULL output;
 *         a bus error from the transfer is returned unchanged.
 */
alp_status_t ina228_read_current_ua(ina228_t *ctx, int32_t *ua_out);

/**
 * @brief Read the power in micro-watts.  Unsigned (SLYS021A eq. 5:
 *        3.2 x CURRENT_LSB x POWER).
 *
 * @param[in]  ctx     Initialised context.
 * @param[out] uw_out  Power, micro-watts.
 * @return ALP_OK; ALP_ERR_NOT_READY for a NULL / uninitialised @p ctx or NULL output;
 *         ALP_ERR_OUT_OF_RANGE if the scaled value does not fit 64 bits (only
 *         with a corrupt context); a bus error from the transfer is returned
 *         unchanged.
 */
alp_status_t ina228_read_power_uw(ina228_t *ctx, uint64_t *uw_out);

/**
 * @brief Read the accumulated energy in micro-joules.  Unsigned 40-bit
 *        register (SLYS021A eq. 6: 16 x 3.2 x CURRENT_LSB x ENERGY).
 *
 * Reading ENERGY clears the ENERGYOF overflow flag.  The register rolls over
 * to zero on overflow.
 *
 * @param[in]  ctx     Initialised context.
 * @param[out] uj_out  Energy, micro-joules.
 * @return ALP_OK; ALP_ERR_NOT_READY for a NULL / uninitialised @p ctx or NULL
 *         output; ALP_ERR_OUT_OF_RANGE if the scaled value does not fit its
 *         result type; a bus error from the transfer is returned unchanged.
 */
alp_status_t ina228_read_energy_uj(ina228_t *ctx, uint64_t *uj_out);

/**
 * @brief Read the accumulated charge in micro-coulombs.  Signed 40-bit
 *        register (SLYS021A eq. 7: CURRENT_LSB x CHARGE).
 *
 * @param[in]  ctx     Initialised context.
 * @param[out] uc_out  Charge, micro-coulombs.
 * @return ALP_OK; ALP_ERR_NOT_READY for a NULL / uninitialised @p ctx or NULL
 *         output; ALP_ERR_OUT_OF_RANGE if the scaled value does not fit its
 *         result type; a bus error from the transfer is returned unchanged.
 */
alp_status_t ina228_read_charge_uc(ina228_t *ctx, int64_t *uc_out);

/**
 * @brief Clear the ENERGY and CHARGE accumulators (CONFIG.RSTACC).
 *
 * @param[in,out] ctx  Initialised context.
 * @return ALP_OK; ALP_ERR_NOT_READY for a NULL / uninitialised @p ctx.
 */
alp_status_t ina228_reset_accumulators(ina228_t *ctx);

/**
 * @brief Read the DIAG_ALRT register (0x0B) raw.
 *
 * Test the result against the ::INA228_DIAG_CNVRF, ::INA228_DIAG_MATHOF,
 * ::INA228_DIAG_ENERGYOF ... masks.  With ALATCH set the latched alert flags
 * clear on this read; the driver leaves ALATCH at its reset value (clear).
 *
 * @param[in]  ctx        Initialised context.
 * @param[out] flags_out  Raw DIAG_ALRT value.
 * @return ALP_OK; ALP_ERR_NOT_READY for a NULL / uninitialised @p ctx or NULL output;
 *         a bus error from the transfer is returned unchanged.
 */
alp_status_t ina228_read_diag(ina228_t *ctx, uint16_t *flags_out);

/**
 * @brief Soft-reset the part (CONFIG.RST) and re-apply ADC range and SHUNT_CAL.
 *
 * ADC_CONFIG returns to its reset default; call ina228_configure() again if
 * a different setup was in use.
 *
 * @param[in,out] ctx  Initialised context.
 * @return ALP_OK; ALP_ERR_NOT_READY for a NULL / uninitialised @p ctx; a bus error.
 */
alp_status_t ina228_reset(ina228_t *ctx);

/**
 * @brief Clear the context.  The bus handle stays open and owned by the caller.
 *
 * @param[in,out] ctx  Context to clear; NULL is a no-op.
 */
void ina228_deinit(ina228_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ALP_CHIPS_INA228_H */
