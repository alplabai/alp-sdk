/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tps628640.h
 * @brief TI TPS62864 / TPS62866 single-channel I2C-controlled buck.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * The TPS62864 family is a 6-A / 7-A buck with two switchable VOUT
 * registers (selected at runtime by the VID pin), an FPWM-mode
 * forcing bit, HICCUP / latching short-circuit protection, and a
 * software-enable bit.  The startup output voltage **and** the I2C
 * slave address are both selected by a single external resistor at
 * the VSET/VID pin; see `Table 8-1` of the datasheet
 * (SLVSEI1C, October 2020).  Available slave addresses span
 * `0x40..0x4F` (7-bit).
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
 * @par Register table (TPS62864 datasheet §8.6, SLVSEI1C)
 *
 * | Addr   | Name              | Access | Default | Purpose                                       |
 * |--------|-------------------|--------|---------|-----------------------------------------------|
 * | `0x01` | VOUT Register 1   | R/W    | 0x64    | Output-voltage setpoint (VID = 0 selects it)  |
 * | `0x02` | VOUT Register 2   | R/W    | 0x64    | Output-voltage setpoint (VID = 1 selects it)  |
 * | `0x03` | CONTROL           | W      | 0x6E    | Reset / enable / FPWM / discharge / ramp speed |
 * | `0x05` | STATUS            | R      | 0x00    | UVLO / HICCUP / thermal warning latches       |
 *
 * VOUT byte encoding: `mv = byte * 5 + 400`.  Range
 * `0x00` (400 mV) .. `0xFF` (1675 mV).  Codes
 * `0x20, 0x40, 0x60, 0x80, 0xC4, 0xE0` carry wider accuracy
 * tolerance per the datasheet's table-8-4 footnote -- avoid in
 * production firmware that needs the tighter VOS accuracy bar.
 *
 * @par CONTROL register bit layout (write-only)
 *
 * Reading 0x03 returns 0x00 per the datasheet ("attempting to read
 * data from register addresses not listed in this section results
 * in 00h being read out", §8.6.1).  Driver maintains a software
 * shadow (@c control_shadow) so the typed helpers below can do
 * read-modify-write semantics without an I2C round-trip.
 *
 * | Bit   | Field                                  | Default | Notes                                      |
 * |-------|----------------------------------------|---------|--------------------------------------------|
 * | 7     | Reset                                  | 0       | 1 = factory reset every register            |
 * | 6     | Enable FPWM during VID change          | 1       | 0 = honour current mode while ramping       |
 * | 5     | Software Enable Device                 | 1       | 0 = disable, registers preserved            |
 * | 4     | Enable FPWM Mode                       | 0       | 1 = force PWM even at light loads           |
 * | 3     | Enable Output Discharge                | 1       | 1 = active discharge resistor on disable    |
 * | 2     | Enable HICCUP                          | 1       | 0 = latching protection (won't auto-recover)|
 * | 1:0   | Voltage Ramp Speed                     | 11      | 00=20mV/us 01=10mV/us 10=5mV/us 11=1mV/us   |
 *
 * @par STATUS register bit layout (read-only; read clears)
 *
 * | Bit  | Field           | Set when                                              |
 * |------|-----------------|-------------------------------------------------------|
 * | 4    | Thermal Warning | Junction temperature crossed 130 °C                   |
 * | 3    | HICCUP          | Device entered HICCUP at least once since last read    |
 * | 0    | UVLO            | Input voltage fell below the UVLO falling threshold    |
 *
 * Latches persist until the register is read; the read action
 * resets all latched bits to 0.
 */

#ifndef ALP_CHIPS_TPS628640_H
#define ALP_CHIPS_TPS628640_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register addresses ----------------------------------------------- */
#define TPS628640_REG_VOUT1 0x01u
#define TPS628640_REG_VOUT2 0x02u
#define TPS628640_REG_CONTROL 0x03u
#define TPS628640_REG_STATUS 0x05u

/* CONTROL bits ----------------------------------------------------- */
#define TPS628640_CTRL_RESET (1u << 7)
#define TPS628640_CTRL_FPWM_DURING_VID_CHANGE (1u << 6)
#define TPS628640_CTRL_SOFTWARE_ENABLE (1u << 5)
#define TPS628640_CTRL_FPWM_MODE (1u << 4)
#define TPS628640_CTRL_OUTPUT_DISCHARGE (1u << 3)
#define TPS628640_CTRL_HICCUP (1u << 2)
#define TPS628640_CTRL_RAMP_SPEED_MASK (0x3u)

/** Datasheet-defined CONTROL register default value (0b01101110).
 *  Driver initialises the shadow to this so the first call to a
 *  typed helper reflects the chip's actual reset state. */
#define TPS628640_CTRL_DEFAULT                                                                     \
    (TPS628640_CTRL_FPWM_DURING_VID_CHANGE | TPS628640_CTRL_SOFTWARE_ENABLE |                      \
     TPS628640_CTRL_OUTPUT_DISCHARGE | TPS628640_CTRL_HICCUP | TPS628640_CTRL_RAMP_SPEED_MASK)

/* STATUS bits ------------------------------------------------------ */
#define TPS628640_STATUS_THERMAL_WARNING (1u << 4)
#define TPS628640_STATUS_HICCUP (1u << 3)
#define TPS628640_STATUS_UVLO (1u << 0)

/** VOUT encoding constants (datasheet §8.6.3). */
#define TPS628640_VOUT_BASE_MV 400u
#define TPS628640_VOUT_MAX_MV 1675u
#define TPS628640_VOUT_STEP_MV 5u

/** Voltage-ramp-speed enum -- CONTROL bits [1:0]. */
typedef enum {
    TPS628640_RAMP_20_MV_PER_US = 0u, /**< 0.25 us/step */
    TPS628640_RAMP_10_MV_PER_US = 1u, /**< 0.5  us/step */
    TPS628640_RAMP_5_MV_PER_US  = 2u, /**< 1    us/step */
    TPS628640_RAMP_1_MV_PER_US  = 3u, /**< 5    us/step  (default) */
} tps628640_ramp_speed_t;

/** Driver context.  Instantiate one per populated TPS628640 on the
 *  bus -- the V2N-M1 populates four (0x44, 0x48, 0x4D, 0x4F). */
typedef struct {
    bool       initialised;
    alp_i2c_t *bus;
    uint8_t    addr;               /**< 7-bit I2C slave address.   */
    uint16_t   default_voltage_mv; /**< Design target for this rail
                                         per `metadata/chips/tps628640.yaml`. */
    uint8_t    control_shadow;     /**< Last CONTROL byte written.
                                         Maintained because the chip's
                                         CONTROL reg is write-only. */
} tps628640_t;

/**
 * @brief Probe the chip at @p addr and record its design-target voltage.
 *
 * Caches the datasheet's default CONTROL byte (`TPS628640_CTRL_DEFAULT`)
 * in @c control_shadow so the typed helpers below can do
 * read-modify-write semantics against the chip's actual reset
 * state.
 *
 * @param ctx                 Driver context.
 * @param bus                 BRD_I2C handle.
 * @param addr_7bit           7-bit slave address (0x40..0x4F per
 *                            the VSET/VID resistor; V2N-M1
 *                            instances at 0x44 / 0x48 / 0x4D / 0x4F).
 * @param default_voltage_mv  Design-target output voltage for this
 *                            instance.  Used only as metadata; the
 *                            chip self-regulates to its R2D-selected
 *                            voltage at power-on.
 * @return ALP_OK / ALP_ERR_NOT_READY (no ACK) / ALP_ERR_INVAL.
 */
alp_status_t tps628640_init(tps628640_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit,
                            uint16_t default_voltage_mv);

/**
 * @brief Set the chip's VOUT1 setpoint in millivolts.
 *
 * Range: 400..1675 mV (5 mV step).  Non-multiple-of-5 inputs round
 * down; read back via @ref tps628640_get_voltage_mv to see what
 * actually landed.
 *
 * @warning  Board-side firmware MUST enforce the rail's design-
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
 * @brief Write VOUT2 (selected when the VID pin is high) in millivolts.
 *
 * Same encoding + safe-window expectations as
 * @ref tps628640_set_voltage_mv.
 */
alp_status_t tps628640_set_voltage2_mv(tps628640_t *ctx, uint16_t mv);

/**
 * @brief Read VOUT2's currently-programmed setpoint in millivolts.
 */
alp_status_t tps628640_get_voltage2_mv(tps628640_t *ctx, uint16_t *mv);

/**
 * @brief Read the latched STATUS register (read-and-clear).
 *
 * Bit layout (datasheet §8.6.6):
 *   - bit 4 = thermal-warning (junction > 130 °C).
 *   - bit 3 = HICCUP entered at least once since last read.
 *   - bit 0 = UVLO active (VIN below falling threshold).
 *
 * STATUS bits latch on event + reset to 0 after this read.
 */
alp_status_t tps628640_get_status(tps628640_t *ctx, uint8_t *status_byte);

/**
 * @brief Toggle the Software Enable bit (CONTROL[5]).
 *
 * Read-modify-write against @c control_shadow because the chip's
 * CONTROL register is write-only.  Setting @p enable to false stops
 * the converter but preserves every register (per datasheet
 * §8.4.10); setting it back to true re-runs soft-start without the
 * usual tDelay.
 */
alp_status_t tps628640_software_enable(tps628640_t *ctx, bool enable);

/**
 * @brief Toggle the Force-PWM bit (CONTROL[4]).
 *
 * @c true forces continuous PWM (lower output ripple, higher Iq);
 * @c false reverts to PFM at light loads (lower Iq, higher ripple).
 * Read-modify-write against @c control_shadow.
 */
alp_status_t tps628640_set_fpwm_mode(tps628640_t *ctx, bool fpwm);

/**
 * @brief Configure the voltage-ramp speed (CONTROL[1:0]).
 *
 * Faster ramp = larger di/dt transient.  Slower ramp = smoother
 * transition but longer settling.  Datasheet default is 1 mV/us
 * (the slowest), which is also the driver's reset shadow.
 */
alp_status_t tps628640_set_ramp_speed(tps628640_t *ctx, tps628640_ramp_speed_t speed);

/**
 * @brief Reset every register to its datasheet default (CONTROL[7] = 1).
 *
 * Equivalent to a power cycle from the I2C interface's perspective.
 * The chip restarts with a tDelay startup, R2D-selected output
 * voltage, default CONTROL byte; the driver shadow re-initialises
 * to @ref TPS628640_CTRL_DEFAULT.
 */
alp_status_t tps628640_reset_to_defaults(tps628640_t *ctx);

/** @brief Raw register read.  Always available; no register-layout dependency. */
alp_status_t tps628640_read_reg(tps628640_t *ctx, uint8_t reg, uint8_t *val);
/** @brief Raw register write.  Always available; no register-layout dependency. */
alp_status_t tps628640_write_reg(tps628640_t *ctx, uint8_t reg, uint8_t val);

/** @brief Release resources.  Idempotent. */
void tps628640_deinit(tps628640_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_TPS628640_H */
