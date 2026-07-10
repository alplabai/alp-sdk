/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pca9451a.h
 * @brief NXP PCA9451A multi-channel power-management IC driver.
 *
 * @par Driver status: [partial-impl] -- probe (ACK-only DEV_ID read) +
 *   raw register R/W + INT1 latched-interrupt decode + per-rail
 *   enable/disable + per-rail voltage set/get (microvolts) on all 6
 *   bucks and 5 LDOs.  NOT yet exercised on real E1M-NX9101 silicon --
 *   that board doesn't exist on the bench yet (issue #474) -- so
 *   treat every number and sequencing decision here as paper-correct
 *   only, validated by the host-side `tests/zephyr/chips` unit suite
 *   against NULL-arg / uninitialised-context / table-bounds
 *   properties, not against a real transceiver.  HiL bring-up happens
 *   once the NX9101 module is on the bench.
 *
 * @par Register-map provenance
 * The PCA9451A shares its register map with the wider NXP PCA9450
 * family (PCA9450A/BC, PCA9451A, PCA9452) -- confirmed by the
 * upstream Linux kernel's own `enum pca9450_chip_type` (NXP-authored,
 * `include/linux/regulator/pca9450.h`), which lists `PCA9450_TYPE_PCA9451A`
 * as a first-class member alongside PCA9450A/BC, and matches this
 * chip's own manifest (`metadata/chips/pca9451a.yaml` `kconfig.yocto`:
 * "kernel pmic-pca9450 driver (PCA9451A is software-compatible per
 * NXP)").  Every register address, bitmask, and voltage-range formula
 * in this driver is taken directly from that GPL-2.0 upstream source
 * (`include/linux/regulator/pca9450.h` + `drivers/regulator/pca9450-regulator.c`,
 * Copyright 2020 NXP) -- not invented, not guessed from the part
 * number.  Where the upstream driver doesn't document a bit's
 * semantics (e.g. per-bit `STATUS1`/`STATUS2` fault decode), this
 * driver deliberately stops at raw register access rather than
 * fabricate a bitmap; only `INT1` (documented `IRQ_*` bits in the
 * same header) gets a decoded accessor.
 *
 * @par I2C addressing
 * Power-on-reset default 7-bit address is `0x25` (Table 17 of the
 * NXP PCA9451A datasheet); the manifest notes the board can
 * reprogram this via OTP, hence @ref pca9451a_init_at.
 *
 * @par Regulator map (BUCK1..6, LDO1..5)
 * | Rail  | Voltage range         | Step     | vsel register    | enable register |
 * |-------|------------------------|----------|-------------------|------------------|
 * | BUCK1 | 0.6..2.1875 V (DVS)   | 12.5 mV  | `BUCK1OUT_DVS0`   | `BUCK1CTRL`      |
 * | BUCK2 | 0.6..2.1875 V (DVS)   | 12.5 mV  | `BUCK2OUT_DVS0`   | `BUCK2CTRL`      |
 * | BUCK3 | 0.6..2.1875 V (DVS)   | 12.5 mV  | `BUCK3OUT_DVS0`   | `BUCK3CTRL`      |
 * | BUCK4 | 0.6..3.4 V            | 25 mV    | `BUCK4OUT`        | `BUCK4CTRL`      |
 * | BUCK5 | 0.6..3.4 V            | 25 mV    | `BUCK5OUT`        | `BUCK5CTRL`      |
 * | BUCK6 | 0.6..3.4 V            | 25 mV    | `BUCK6OUT`        | `BUCK6CTRL`      |
 * | LDO1  | 1.6..1.9 V / 3.0..3.3 V (two bands) | 100 mV | `LDO1CTRL` | `LDO1CTRL` (shared reg) |
 * | LDO2  | 0.8..1.15 V           | 50 mV    | `LDO2CTRL`        | `LDO2CTRL` (shared reg) |
 * | LDO3  | 0.8..3.3 V            | 100 mV   | `LDO3CTRL`        | `LDO3CTRL` (shared reg) |
 * | LDO4  | 0.8..3.3 V            | 100 mV   | `LDO4CTRL`        | `LDO4CTRL` (shared reg) |
 * | LDO5  | 1.8..3.3 V            | 100 mV   | `LDO5CTRL_H`      | `LDO5CTRL_H` (shared reg) |
 *
 * BUCK1-3 additionally have a `DVS1` (standby) setpoint register this
 * driver does not touch -- `_rail_set_voltage_uv` / `_rail_get_voltage_uv`
 * only address the `DVS0` (RUN) setpoint, matching the upstream
 * driver's default (non-DVS-controlled) configuration.  LDO5 has an
 * `_L` / `_H` register pair selected by the board's `SD_VSEL` pin;
 * this driver only implements `_H` (SD_VSEL driven high), matching
 * the upstream Linux driver's own default GPIO handling
 * (`gpiod_get_optional(..., GPIOD_OUT_HIGH)` for `sd-vsel`).  A board
 * that straps `SD_VSEL` low needs the `_L` register instead -- not
 * implemented here; use @ref pca9451a_write_reg / @ref pca9451a_read_reg
 * directly against `PCA9451A_REG_LDO5CTRL_L` if that variant is
 * needed.
 *
 * @par Enable-field convention
 * BUCK enable fields are the 2-bit `ENMODE` (`OFF=00`, `ONREQ=01`,
 * `ONREQ_STBYREQ=10`, `ON=11`); LDO enable fields are a 2-bit field
 * with an undocumented per-value meaning in the upstream header
 * (only the field mask is given, not named states).  This driver
 * follows the Linux regulator core's own default convention for a
 * bare `enable_mask` with no explicit `enable_val`/`disable_val`
 * override (`regulator_enable_regmap`'s default: enable writes the
 * full mask, disable writes zero) -- i.e. @ref pca9451a_rail_set_enable
 * writes the field's mask bits (BUCK: `ON`, always-on) to enable and
 * clears the field to disable.  This is the documented Linux default,
 * not a guess specific to this driver.
 *
 * @par Board-level signals out of scope
 * `PMIC_ON_REQ` / `PMIC_STBY_REQ` / `PMIC_RST_B` / `WDOG_B` / `SW_EN` /
 * `IRQ_B` / `POR_B` / `RTC_RESET_B` / `SD_VSEL` (see
 * `metadata/chips/pca9451a.yaml` `signals:`) are board-routed GPIO
 * pins, not I2C registers -- this driver (like the sibling `act8760`
 * / `da9292` PMIC drivers) is I2C-only and does not open or drive
 * them.  Board firmware manages those pins with `<alp/peripheral.h>`
 * GPIO calls directly.
 *
 * @par Datasheet provenance
 * - **NXP PCA9451A datasheet Rev 2.1 (20 Aug 2025)** -- I2C address,
 *   package/pin data (see `metadata/chips/pca9451a.yaml`).
 * - **Linux kernel `include/linux/regulator/pca9450.h` +
 *   `drivers/regulator/pca9450-regulator.c`** (Copyright 2020 NXP,
 *   GPL-2.0-or-later, `MODULE_AUTHOR("Robin Gong <yibin.gong@nxp.com>")`)
 *   -- register map, bitmasks, voltage-range formulas.  This is the
 *   register-map source of truth for this driver.
 */

#ifndef ALP_CHIPS_PCA9451A_H
#define ALP_CHIPS_PCA9451A_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Power-on-reset default 7-bit I2C slave address (datasheet Table 17).
 *  The board can reprogram this via OTP -- see @ref pca9451a_init_at. */
#define PCA9451A_I2C_ADDR 0x25u

/** @name Register addresses
 *  Verified against the upstream Linux kernel's `enum` in
 *  `include/linux/regulator/pca9450.h` (Copyright 2020 NXP).
 *  @{
 */
#define PCA9451A_REG_DEV_ID        0x00u
#define PCA9451A_REG_INT1          0x01u
#define PCA9451A_REG_INT1_MSK      0x02u
#define PCA9451A_REG_STATUS1       0x03u
#define PCA9451A_REG_STATUS2       0x04u
#define PCA9451A_REG_PWRON_STAT    0x05u
#define PCA9451A_REG_SWRST         0x06u
#define PCA9451A_REG_PWRCTRL       0x07u
#define PCA9451A_REG_RESET_CTRL    0x08u
#define PCA9451A_REG_CONFIG1       0x09u
#define PCA9451A_REG_CONFIG2       0x0Au
#define PCA9451A_REG_BUCK123_DVS   0x0Cu
#define PCA9451A_REG_BUCK1CTRL     0x10u
#define PCA9451A_REG_BUCK1OUT_DVS0 0x11u
#define PCA9451A_REG_BUCK1OUT_DVS1 0x12u
#define PCA9451A_REG_BUCK2CTRL     0x13u
#define PCA9451A_REG_BUCK2OUT_DVS0 0x14u
#define PCA9451A_REG_BUCK2OUT_DVS1 0x15u
#define PCA9451A_REG_BUCK3CTRL     0x16u
#define PCA9451A_REG_BUCK3OUT_DVS0 0x17u
#define PCA9451A_REG_BUCK3OUT_DVS1 0x18u
#define PCA9451A_REG_BUCK4CTRL     0x19u
#define PCA9451A_REG_BUCK4OUT      0x1Au
#define PCA9451A_REG_BUCK5CTRL     0x1Bu
#define PCA9451A_REG_BUCK5OUT      0x1Cu
#define PCA9451A_REG_BUCK6CTRL     0x1Du
#define PCA9451A_REG_BUCK6OUT      0x1Eu
#define PCA9451A_REG_LDO_AD_CTRL   0x20u
#define PCA9451A_REG_LDO1CTRL      0x21u
#define PCA9451A_REG_LDO2CTRL      0x22u
#define PCA9451A_REG_LDO3CTRL      0x23u
#define PCA9451A_REG_LDO4CTRL      0x24u
#define PCA9451A_REG_LDO5CTRL_L    0x25u
#define PCA9451A_REG_LDO5CTRL_H    0x26u
#define PCA9451A_REG_LOADSW_CTRL   0x2Au
#define PCA9451A_REG_VRFLT1_STS    0x2Bu
#define PCA9451A_REG_VRFLT2_STS    0x2Cu
#define PCA9451A_REG_VRFLT1_MASK   0x2Du
#define PCA9451A_REG_VRFLT2_MASK   0x2Eu
#define PCA9451A_MAX_REGISTER      0x2Fu
/** @} */

/** @name INT1 bits (datasheet-documented; upstream `IRQ_*` macros).
 *  @{
 */
#define PCA9451A_INT1_PWRON     0x80u
#define PCA9451A_INT1_WDOGB     0x40u
#define PCA9451A_INT1_RSVD      0x20u
#define PCA9451A_INT1_VR_FLT1   0x10u
#define PCA9451A_INT1_VR_FLT2   0x08u
#define PCA9451A_INT1_LOWVSYS   0x04u
#define PCA9451A_INT1_THERM_105 0x02u
#define PCA9451A_INT1_THERM_125 0x01u
/** @} */

/** BUCK `ENMODE` field values (2-bit; upstream `BUCK_ENMODE_*`). */
typedef enum {
	PCA9451A_ENMODE_OFF           = 0x00, /**< Always off. */
	PCA9451A_ENMODE_ONREQ         = 0x01, /**< On when `PMIC_ON_REQ` asserted. */
	PCA9451A_ENMODE_ONREQ_STBYREQ = 0x02, /**< On when ON_REQ or STBY_REQ asserted. */
	PCA9451A_ENMODE_ON            = 0x03, /**< Always on (regulator-core default enable). */
} pca9451a_enmode_t;

/** Per-regulator identifier for the rail-level helpers below. */
typedef enum {
	PCA9451A_RAIL_BUCK1 = 0,
	PCA9451A_RAIL_BUCK2,
	PCA9451A_RAIL_BUCK3,
	PCA9451A_RAIL_BUCK4,
	PCA9451A_RAIL_BUCK5,
	PCA9451A_RAIL_BUCK6,
	PCA9451A_RAIL_LDO1,
	PCA9451A_RAIL_LDO2,
	PCA9451A_RAIL_LDO3,
	PCA9451A_RAIL_LDO4,
	PCA9451A_RAIL_LDO5,
	PCA9451A_RAIL_COUNT
} pca9451a_rail_t;

/** Decoded `INT1` latched-interrupt snapshot.  Per the upstream
 *  driver's own IRQ handler, `INT1` is read once per assertion and
 *  the datasheet documents it as read-to-acknowledge -- reading it
 *  here has the same latch-clearing side effect. */
typedef struct {
	bool    pwron;     /**< bit7: `PMIC_ON_REQ` edge. */
	bool    wdogb;     /**< bit6: watchdog (`WDOG_B`) event. */
	bool    vr_flt1;   /**< bit4: a `VRFLT1_STS` regulator fault is set. */
	bool    vr_flt2;   /**< bit3: a `VRFLT2_STS` regulator fault is set. */
	bool    low_vsys;  /**< bit2: system supply below the LOWVSYS threshold. */
	bool    therm_105; /**< bit1: junction crossed 105 degC warning. */
	bool    therm_125; /**< bit0: junction crossed 125 degC critical. */
	uint8_t raw;       /**< Untouched INT1 byte for diagnostics. */
} pca9451a_status_t;

/** Driver context. */
typedef struct {
	bool       initialised;
	alp_i2c_t *bus;
	uint8_t    addr;   /**< 7-bit I2C slave address (default 0x25). */
	uint8_t    dev_id; /**< Cached DEV_ID byte, read at init (diagnostics only --
                             this driver does not assert an expected value; the
                             exact DEV_ID nibble for the PCA9451A variant isn't
                             confirmed against a source this driver can cite). */
} pca9451a_t;

/**
 * @brief Probe the PMIC at the default address and initialise the context.
 *
 * Reads `DEV_ID` (register 0x00) purely as an ACK check -- like the
 * sibling `act8760_init`, this does not assert a specific device-ID
 * value (the exact expected nibble for the PCA9451A variant isn't
 * confirmed against a source this driver can cite; the upstream
 * Linux driver only validates it for the PCA9450A/BC types it
 * explicitly branches on).  The read byte is cached in `ctx->dev_id`
 * for the caller to log/inspect.  Does not alter any regulator state.
 *
 * @param ctx  Driver context (output).
 * @param bus  I2C bus handle.
 * @return     ALP_OK on success, ALP_ERR_NOT_READY if the slave
 *             doesn't ACK, ALP_ERR_INVAL on NULL args.
 */
alp_status_t pca9451a_init(pca9451a_t *ctx, alp_i2c_t *bus);

/**
 * @brief Initialise with a non-default I2C address (board reprogrammed
 *        the address via OTP).
 *
 * @param ctx        Driver context (output).
 * @param bus        I2C bus handle.
 * @param addr_7bit  7-bit slave address.
 * @return           ALP_OK on success, ALP_ERR_NOT_READY if the slave
 *                    doesn't ACK, ALP_ERR_INVAL on NULL args or addr=0.
 */
alp_status_t pca9451a_init_at(pca9451a_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit);

/**
 * @brief Read and decode the `INT1` latched-interrupt register.
 *
 * @note  Reading `INT1` clears its latched bits (datasheet
 *        read-to-acknowledge semantics, mirrored by the upstream
 *        Linux driver's IRQ handler reading it once per assertion).
 *
 * @param ctx  Context (must be initialised).
 * @param out  Decoded status (output).
 * @return     ALP_OK, ALP_ERR_NOT_READY if uninitialised,
 *             ALP_ERR_INVAL on NULL @p out.
 */
alp_status_t pca9451a_get_status(pca9451a_t *ctx, pca9451a_status_t *out);

/**
 * @brief Enable or disable a regulator rail.
 *
 * Read-modify-write against the rail's enable field (see the
 * regulator map table above); preserves the other bits sharing the
 * register (LDO vsel + enable share one byte).  Per the
 * @par Enable-field convention documented above, enabling writes the
 * full enable-field mask (BUCK: `ON`/always-on); disabling clears it.
 *
 * @param ctx     Context (must be initialised).
 * @param rail    Which regulator.
 * @param enable  true = enable, false = disable.
 * @return        ALP_OK, ALP_ERR_NOT_READY if uninitialised,
 *                ALP_ERR_INVAL if @p rail is out of range.
 */
alp_status_t pca9451a_rail_set_enable(pca9451a_t *ctx, pca9451a_rail_t rail, bool enable);

/**
 * @brief Read a regulator rail's enable state.
 *
 * A rail reads as enabled if its enable field is non-zero (BUCK:
 * anything but `ENMODE_OFF`; LDO: the field's undocumented non-zero
 * states -- see the enable-field convention note above).
 *
 * @param ctx      Context (must be initialised).
 * @param rail     Which regulator.
 * @param enabled  Output.
 * @return         ALP_OK, ALP_ERR_NOT_READY if uninitialised,
 *                 ALP_ERR_INVAL on NULL @p enabled or out-of-range @p rail.
 */
alp_status_t pca9451a_rail_is_enabled(pca9451a_t *ctx, pca9451a_rail_t rail, bool *enabled);

/**
 * @brief Program a regulator rail's output voltage.
 *
 * Converts @p microvolts to the rail's raw `vsel` code using the
 * verified linear-range formula from the upstream driver (see the
 * regulator map table above), then read-modify-writes the `vsel`
 * register (preserving the enable-field bits where the two share a
 * byte).  Values outside the rail's documented range are clamped to
 * the nearest supported code by the same rule the upstream driver's
 * `REGULATOR_LINEAR_RANGE` fixed-tail entries use (e.g. BUCK4/5/6
 * clamp anything >= 3.4 V to the 3.4 V code) -- values below the
 * rail's floor return ALP_ERR_OUT_OF_RANGE rather than silently
 * clamping up, since under-shooting a supply is the more dangerous
 * failure mode for a PMIC caller to have silently absorbed.
 *
 * @param ctx        Context (must be initialised).
 * @param rail       Which regulator.
 * @param microvolts Target output voltage, in microvolts.
 * @return           ALP_OK, ALP_ERR_NOT_READY if uninitialised,
 *                   ALP_ERR_INVAL if @p rail is out of range,
 *                   ALP_ERR_OUT_OF_RANGE if @p microvolts is below
 *                   the rail's documented floor.
 */
alp_status_t
pca9451a_rail_set_voltage_uv(pca9451a_t *ctx, pca9451a_rail_t rail, int32_t microvolts);

/**
 * @brief Read back a regulator rail's programmed output voltage.
 *
 * Inverse of @ref pca9451a_rail_set_voltage_uv -- decodes the raw
 * `vsel` code through the same verified linear-range formula.
 *
 * @param ctx        Context (must be initialised).
 * @param rail       Which regulator.
 * @param microvolts Output, in microvolts.
 * @return           ALP_OK, ALP_ERR_NOT_READY if uninitialised,
 *                   ALP_ERR_INVAL on NULL @p microvolts or
 *                   out-of-range @p rail.
 */
alp_status_t
pca9451a_rail_get_voltage_uv(pca9451a_t *ctx, pca9451a_rail_t rail, int32_t *microvolts);

/**
 * @brief Raw register read (diagnostics / advanced use).
 *
 * @param ctx  Context (must be initialised).
 * @param reg  8-bit register address.
 * @param out  Output byte.
 * @return     ALP_OK, ALP_ERR_NOT_READY if uninitialised,
 *             ALP_ERR_INVAL on NULL @p out.
 */
alp_status_t pca9451a_read_reg(pca9451a_t *ctx, uint8_t reg, uint8_t *out);

/**
 * @brief Raw register write (diagnostics / advanced use).
 *
 * @warning  Prefer the typed rail helpers above.  Writing
 *           `RESET_CTRL` / `SWRST` / `CONFIG1` / `CONFIG2` directly
 *           can change watchdog-reset behaviour or trigger a chip
 *           reset -- this driver deliberately does not wrap those in
 *           a typed helper (untested, unvalidated on real silicon).
 *
 * @param ctx  Context (must be initialised).
 * @param reg  8-bit register address.
 * @param val  Byte to write.
 * @return     ALP_OK, ALP_ERR_NOT_READY if uninitialised.
 */
alp_status_t pca9451a_write_reg(pca9451a_t *ctx, uint8_t reg, uint8_t val);

/**
 * @brief Release resources.  Idempotent.
 *
 * @param ctx  Context (may be NULL).
 */
void pca9451a_deinit(pca9451a_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_PCA9451A_H */
