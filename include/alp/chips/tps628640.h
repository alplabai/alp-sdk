/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tps628640.h
 * @brief TI TPS628640 / TPS628641 / TPS628642 single-channel buck driver
 *        (multi-instance scaffold).
 *
 * @par Populated instances on V2N-M1 (BRD_I2C)
 *
 *   | Address | Voltage  | Rail name           | Population scope          |
 *   |---------|----------|---------------------|---------------------------|
 *   | `0x48`  | 0.85 V   | `VDD0V85_LPDDR`     | DEEPX LPDDR + DDR core    |
 *   | `0x44`  | 1.05 V   | `DDR5_VDD` (L+H)    | DEEPX DDR5 VDD bank       |
 *   | `0x4F`  | 0.50 V   | `DDR5_VDDQ_0V5`     | DEEPX DDR5 IO termination |
 *   | `0x4D`  | 0.60 V   | `LPD4x_0V6`         | **Renesas** LPDDR4X       |
 *
 * `0x4D` is `assembled: optional` on the V2N base SoM (when
 * unpopulated, the Renesas LPDDR4X 0.6 V supply comes from ACT8760
 * or DA9292 instead).  The other three are V2N-M1-only since they
 * power DEEPX-specific rails.
 *
 * @par Register layout (TPS62864 datasheet SLVSEI1C)
 *
 * | Addr | Name    | Access | Purpose                                       |
 * |------|---------|--------|-----------------------------------------------|
 * | 0x01 | VOUT1   | R/W    | Output-voltage setpoint, register 1           |
 * | 0x02 | VOUT2   | R/W    | Output-voltage setpoint, register 2 (VID pin) |
 * | 0x03 | CONTROL | W      | Operating-mode + ramp + reset                  |
 * | 0x05 | STATUS  | R      | UVLO + HICCUP + thermal-warning latches        |
 *
 * VOUT byte encoding: `mv = byte * 5 + 400`.  Range
 * `0x00` (400 mV) .. `0xFF` (1675 mV).  Set / get / status helpers
 * implement this encoding directly.  CONTROL writes go through the
 * raw register helper since the SDK's typical use-case doesn't
 * require runtime CONTROL changes (FPWM forcing, HICCUP disable,
 * etc.) -- carriers that need them write the bits directly.
 *
 * The VID pin selects between VOUT1 and VOUT2 at runtime; carriers
 * that hold VID statically can use either register.  This driver
 * writes VOUT1 by default; raw R/W is available for VOUT2 if
 * needed.
 */

#ifndef ALP_CHIPS_TPS628640_H
#define ALP_CHIPS_TPS628640_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Driver context.  Instantiate one per populated TPS628640 on the
 *  bus -- the V2N-M1 populates four (0x44, 0x48, 0x4D, 0x4F). */
typedef struct {
    bool       initialised;
    alp_i2c_t *bus;
    uint8_t    addr;                /**< 7-bit I2C slave address.   */
    uint16_t   default_voltage_mv;  /**< Design target for this rail
                                         per `metadata/chips/tps628640.yaml`. */
} tps628640_t;

/**
 * @brief Probe the chip at @p addr and record its design-target voltage.
 *
 * @param ctx                 Driver context.
 * @param bus                 BRD_I2C handle.
 * @param addr_7bit           7-bit slave address (e.g. 0x44, 0x48,
 *                            0x4D, 0x4F for the four V2N-M1 rails).
 * @param default_voltage_mv  Design-target output voltage for this
 *                            instance.  Used only as metadata; the
 *                            chip self-regulates to its OTP voltage
 *                            at power-on.
 * @return ALP_OK / ALP_ERR_NOT_READY (no ACK) / ALP_ERR_INVAL.
 */
alp_status_t tps628640_init(tps628640_t *ctx, alp_i2c_t *bus,
                            uint8_t addr_7bit, uint16_t default_voltage_mv);

/**
 * @brief Set the chip's VOUT1 setpoint in millivolts.
 *
 * Range: 400..1675 mV (5 mV step).  Non-multiple-of-5 inputs round
 * down; read back via @ref tps628640_get_voltage_mv to see what
 * actually landed.
 *
 * @warning  Carrier-side firmware MUST enforce the rail's design-
 *           safe operating window before calling this -- e.g. the
 *           V2N-M1 `DDR5_VDDQ` instance at I²C `0x4F` targets
 *           0.5 V and a write to 1 V would damage downstream
 *           silicon.  The window lives in
 *           `metadata/chips/tps628640.yaml` per instance plus the
 *           design-target voltage cached in `ctx`.
 *
 * @return ALP_OK / ALP_ERR_OUT_OF_RANGE (mv outside 400..1675) /
 *         ALP_ERR_NOT_READY (uninitialised) / ALP_ERR_IO.
 */
alp_status_t tps628640_set_voltage_mv(tps628640_t *ctx, uint16_t mv);

/**
 * @brief Read the live VOUT1 setpoint in millivolts.
 *
 * Decodes the register byte through the same `mv = byte * 5 + 400`
 * formula `_set_voltage_mv` uses.
 */
alp_status_t tps628640_get_voltage_mv(tps628640_t *ctx, uint16_t *mv);

/**
 * @brief Read the latched STATUS register (read-and-clear).
 *
 * Bit layout (from TPS62864 datasheet §8.6.6):
 *   - bit 4 = thermal-warning (junction > 130 °C).
 *   - bit 3 = HICCUP entered at least once since last read.
 *   - bit 0 = UVLO active (VIN below falling threshold).
 *
 * STATUS bits latch on event + reset to 0 after this read.
 */
alp_status_t tps628640_get_status(tps628640_t *ctx, uint8_t *status_byte);

/** Raw register R/W (always available; no register-layout dependency). */
alp_status_t tps628640_read_reg(tps628640_t *ctx, uint8_t reg, uint8_t *val);
alp_status_t tps628640_write_reg(tps628640_t *ctx, uint8_t reg, uint8_t val);

/** @brief Release resources. */
void         tps628640_deinit(tps628640_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_TPS628640_H */
