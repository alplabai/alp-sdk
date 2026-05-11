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
 * @par Driver status: STUB
 *
 * The TPS628640 datasheet is **not** in the OneDrive datasheet tree
 * that backs this repo's chip drivers; this driver therefore lands
 * as a stub.  It implements:
 *
 *   - `tps628640_init(ctx, bus, addr, default_voltage_mv)` -- ACK
 *     probe + caches the rail's design-target voltage as metadata.
 *   - Raw register read / write.
 *
 * Everything voltage-related (`tps628640_set_voltage_mv`,
 * `tps628640_get_voltage_mv`, `tps628640_get_status`) returns
 * `ALP_ERR_NOSUPPORT` pending datasheet confirmation of the VSET
 * register layout.  When the maintainer adds the TI datasheet to
 * OneDrive (TPS628640 / TPS628641 / TPS628642 -- the silicon variants
 * differ in default voltage but share the I2C surface), fill in the
 * register map in `chips/tps628640/tps628640.c` and switch the
 * helper bodies from NOSUPPORT to real implementations.
 *
 * Until then carriers should treat these rails as factory-default
 * outputs of the TI silicon -- the chip self-regulates to its OTP
 * voltage at power-up and the host shouldn't touch it.
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
 * @brief Set the chip's output voltage (millivolts).
 *
 * @note  Stub: returns `ALP_ERR_NOSUPPORT` pending datasheet
 *        confirmation of the VSET register layout.
 *
 * @warning  When implemented, must enforce the rail's documented
 *           safe-operating window (e.g. DDR5_VDDQ is a 0.5 V rail
 *           and pushing it to 1 V would damage downstream silicon).
 *           The window comes from `metadata/chips/tps628640.yaml`
 *           plus the design-target voltage stored in `ctx`.
 */
alp_status_t tps628640_set_voltage_mv(tps628640_t *ctx, uint16_t mv);

/**
 * @brief Read the live output voltage setpoint in millivolts.
 * @note  Stub: returns `ALP_ERR_NOSUPPORT`. */
alp_status_t tps628640_get_voltage_mv(tps628640_t *ctx, uint16_t *mv);

/**
 * @brief Read fault / status flags.
 * @note  Stub: returns `ALP_ERR_NOSUPPORT`. */
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
