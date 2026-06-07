/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file act8760.h
 * @brief Qorvo / Active-Semi ACT88760 primary PMIC driver.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * @par Verification status: [REGISTER-MAP-VERIFIED 2026-06-06]
 *   Per-rail VSET0 offsets, the two-slave address model, and the
 *   system-status bit map were extracted from the AA82BZ register-map
 *   workbook (AA82BZ_RegisterMap_Users_Rev1P1) and the ACT88760
 *   Datasheet Rev C, then independently re-derived cell-by-cell.
 *   Still [UNTESTED] on silicon -- see the HiL caveat above.
 *
 * The V2N / V2N-M1 SoMs populate the **ACT88760-120.E1** variant
 * (Code Matrix Index 120, revision E1).  The configuration ID is
 * stamped in the chip's non-volatile memory and is what determines
 * the per-rail default voltages, GPIO assignments, power-on sequence,
 * and which I2C slave-address pair the IC responds to.  The CMI is
 * programmed at the factory or via Qorvo's `ActiveCiPS` dongle from
 * an `.iact` profile; runtime I2C lets the host **tweak** values but
 * cannot rewrite the CMI (rewriting needs the dongle's high-voltage
 * programming pulse on the GPIO5 pin).
 *
 * @par Capacity
 * - 7x DC/DC step-down bucks (Buck1..Buck6 on ADD1, Buck7 on ADD2)
 * - 6x LDOs (LDO1..LDO6 on ADD2; LDO5/6 can be configured as
 *   load-switches)
 * - 11x configurable GPIOs (GPIO1..GPIO11) on ADD1
 * - VIO reference (1.62..1.8 V), nIRQ, nRESET, nPB, PWREN/PWRON pins
 *
 * @par I2C slave addressing
 * The chip presents **two independent 7-bit slave addresses** (NOT
 * register pages within one slave).  ADD1 (selected by the CMI via
 * MSTR register 0x16[7:6]) hosts the MSTR tile + GPIO tile + Buck1..6
 * tiles.  ADD2 (MSTR register 0x23[4:3]) hosts the Buck7 tile +
 * dual-LDO tiles LDO12, LDO53, LDO64.
 *
 * The V2N schematic populates ADD1 as `0x25` and ADD2 as `0x26`; both
 * live on BRD_I2C (Renesas RIIC8, master pads `P07` / `P06`).  All
 * four documented CMI address pairs are adjacent (0x25/0x26,
 * 0x27/0x28, 0x67/0x68, 0x6B/0x6C), so `act8760_init_at`'s
 * `page0 + 1` derivation holds for every variant.
 *
 * @par Per-rail voltage selection
 * Bucks: 7-bit VSET, VOUT = 500 mV + VSET x 5 mV (Output-Low range)
 * or x 25 mV (Output-High); the active range is the rail's Vout_Range
 * bit (Buck5/6 are Output-High only).  LDO1/2: 6-bit VSET, 12.5 mV
 * step, 0.5-1.2875 V (RANGE=0) or 1.2-1.9875 V (RANGE=1).  LDO3-6:
 * 0.5-1.2875 V or 1-4.15 V.  This driver exposes the raw VSET value;
 * mV conversion needs the rail's range bit, which raw `read_reg` can
 * fetch -- a typed mV helper lands when a consumer needs one (YAGNI).
 * Buck1/2/7 additionally have VSET2/VSET3 DVS slots bank-aliased onto
 * the VSET0/1 addresses via MSTR 0x2C bit0 (BAND_SEL) -- out of this
 * driver's scope.
 *
 * @par Status surface
 * - System status is decoded from register 0x00 on the ADD1 slave
 *   (MSTR tile).  The byte carries eight per-system flags; per-
 *   regulator POK/OV/ILIM flags live in each tile's offset-0 register
 *   (reachable via act8760_read_reg).
 * - Thermal warning bit `TWARN` (bit 5 of 0x00): the IC asserts
 *   `nIRQ` and (unless masked) shuts down when the junction crosses
 *   165 degC.
 * - VSYSSTAT (bit 4) and VSYSWARN (bit 1) latch on the AVIN falling
 *   edge and clear on read of register 0x00.
 *
 * @par Datasheet provenance
 * - **ACT88760 Datasheet Rev C, August 5, 2025** (Qorvo / Active-Semi,
 *   NDA-protected) -- block diagram + register-naming conventions.
 * - **AA82BZ_RegisterMap_Users_Rev1P1 Customer Facing.xlsx** --
 *   per-bit register map; source of truth for VSET0 offsets + status
 *   byte decode (verified cell-by-cell 2026-06-06).
 * - **ACT88760 Users Guide Rev 3.0** -- VSET-to-mV tables per range.
 * - **ACT88760 ActiveCiPS Programming Guide Rev 0.1** -- CMI flow.
 * - **ACT88760_CMI120 Power Sequence_250916.pdf** -- the V2N power-on
 *   timing diagram + per-rail target voltage list for CMI 120.E1.
 *   This driver does NOT encode the rail names from that PDF in code
 *   -- board-board firmware reads them from `metadata/chips/act8760.yaml`
 *   instead (the rail manifest), so a CMI swap doesn't require a code
 *   rebuild.
 *
 * @par Operating mode
 * - During normal operation the host's role is **read-only telemetry +
 *   error reporting**: poll `act8760_get_status`, react on faults,
 *   read VSET back to confirm a rail came up to its programmed value.
 * - DVS (dynamic voltage scaling) on Buck1/2/7's VSET1..VSET3 slots is
 *   OUT of this driver's scope (`act8760_rail_set_vset` programs VSET0
 *   only); callers needing DVS drive the slot registers and the
 *   `BkxDvsI2C` bits in MSTR register `0x2C` via the raw
 *   `act8760_write_reg` helpers.
 * - **Volatile vs non-volatile:** I2C writes only update the volatile
 *   shadow.  Power-cycling reverts to the CMI defaults.  This is
 *   intentional -- the CMI is the source of truth for the production
 *   power tree.
 */

#ifndef ALP_CHIPS_ACT8760_H
#define ALP_CHIPS_ACT8760_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** ADD1 I2C 7-bit slave address for CMI 120.E1 (MSTR + Buck1..6
 *  + GPIOs).  Matches the V2N schematic. */
#define ACT8760_I2C_ADDR_PAGE0 0x25u

/** ADD2 I2C 7-bit slave address for CMI 120.E1 (Buck7 + LDO1..6).
 *  Lives at PAGE0+1 by chip design for all documented CMI variants. */
#define ACT8760_I2C_ADDR_PAGE1 0x26u

/** Slave selector for raw register access. */
typedef enum {
    ACT8760_PAGE_SYSTEM = 0, /**< ADD1: MSTR + Buck1..Buck6 + GPIOs (0x25 on CMI 120.E1). */
    ACT8760_PAGE_AUX    = 1, /**< ADD2: Buck7 + LDO1..LDO6 (0x26 on CMI 120.E1). */
} act8760_page_t;

/** Per-regulator identifier used by the high-level voltage / status
 *  helpers.  The numeric values are arbitrary; do not assume they
 *  match silicon register indices. */
typedef enum {
    ACT8760_RAIL_BUCK1 = 0,
    ACT8760_RAIL_BUCK2,
    ACT8760_RAIL_BUCK3,
    ACT8760_RAIL_BUCK4,
    ACT8760_RAIL_BUCK5,
    ACT8760_RAIL_BUCK6,
    ACT8760_RAIL_BUCK7,
    ACT8760_RAIL_LDO1,
    ACT8760_RAIL_LDO2,
    ACT8760_RAIL_LDO3,
    ACT8760_RAIL_LDO4,
    ACT8760_RAIL_LDO5,
    ACT8760_RAIL_LDO6,
    ACT8760_RAIL_COUNT
} act8760_rail_t;

/** Decoded system status (register 0x00 on the ADD1 slave).  Bit map
 *  per the AA82BZ MSTR sheet (verified 2026-06-06).  VSYSSTAT and
 *  VSYSWARN latch on the VIN falling edge and clear on read; per-
 *  regulator POK/OV/ILIM flags live in each tile's offset-0 register
 *  (reachable via act8760_read_reg), NOT in this byte. */
typedef struct {
    bool    rom_stat;        /**< bit7: ROM/CMI configuration-load status. */
    bool    wd_alert;        /**< bit6: watchdog timer alert. */
    bool    thermal_warning; /**< bit5 TWARN: junction at warning threshold. */
    bool    vsys_stat;       /**< bit4: AVIN below VSYS_MON (latched). */
    bool    vin_pok_ov;      /**< bit3: VIN power-OK / over-voltage flag. */
    bool    pb_assert;       /**< bit2: push-button assert event. */
    bool    vsys_warning;    /**< bit1: AVIN below VSYS_WARN (latched). */
    bool    pb_deassert;     /**< bit0: push-button de-assert event. */
    uint8_t raw;             /**< Untouched register byte for diagnostics. */
} act8760_status_t;

/** Driver context. */
typedef struct {
    bool       initialised;
    alp_i2c_t *bus;
    uint8_t    addr_page0; /**< Defaults to ACT8760_I2C_ADDR_PAGE0. */
    uint8_t    addr_page1; /**< Defaults to ACT8760_I2C_ADDR_PAGE1. */
} act8760_t;

/**
 * @brief Probe both slaves of the PMIC and initialise the context.
 *
 * Reads register 0x00 on the ADD1 slave (system status) and register
 * 0x00 on the ADD2 slave (Buck7 tile status byte) to confirm the IC
 * ACKs at the expected address pair.  Does **not** alter the volatile
 * shadow registers -- the IC starts up in the CMI default state and
 * the host is expected to leave power-tree config alone for normal
 * operation.
 *
 * @param ctx   Driver context (output).
 * @param bus   I2C bus handle for BRD_I2C.
 * @return      ALP_OK on success, ALP_ERR_NOT_READY if either slave
 *              fails to ACK, ALP_ERR_INVAL on NULL args.
 */
alp_status_t act8760_init(act8760_t *ctx, alp_i2c_t *bus);

/**
 * @brief Initialise with non-default I2C addresses (for boards that
 *        populate a different CMI variant -- 0x27/0x28, 0x67/0x68,
 *        0x6B/0x6C).
 *
 * @param ctx         Driver context (output).
 * @param bus         I2C bus handle for BRD_I2C.
 * @param addr_page0  ADD1 7-bit address.  ADD2 is assumed to be at
 *                    `addr_page0 + 1` per chip design (holds for all
 *                    documented CMI address pairs).
 * @return            ALP_OK on success, ALP_ERR_NOT_READY if either
 *                    slave fails to ACK, ALP_ERR_INVAL on NULL args or
 *                    invalid address.
 */
alp_status_t act8760_init_at(act8760_t *ctx, alp_i2c_t *bus, uint8_t addr_page0);

/**
 * @brief Read the system-status register (0x00, ADD1) and decode
 *        the status bits into @p out.
 *
 * @note  Reading the register **also clears** the latched VSYSSTAT and
 *        VSYSWARN bits per the datasheet's "I2C read clears latch"
 *        semantics.  Call this periodically or in response to a nIRQ
 *        assertion.
 *
 * @param ctx   ACT8760 context handle (must be initialised first).
 * @param out   Pointer to the decoded status structure.
 * @return      ALP_OK on success, ALP_ERR_NOT_READY if not initialised,
 *              ALP_ERR_INVAL on NULL args.
 */
alp_status_t act8760_get_status(act8760_t *ctx, act8760_status_t *out);

/**
 * @brief Raw register read on either slave.
 *
 * @param ctx   ACT8760 context handle (must be initialised first).
 * @param page  Which slave address hosts the register.
 * @param reg   8-bit register address.
 * @param out   Pointer to the destination byte.
 * @return      ALP_OK on success, ALP_ERR_NOT_READY if not initialised,
 *              ALP_ERR_INVAL on NULL / invalid args.
 */
alp_status_t act8760_read_reg(act8760_t *ctx, act8760_page_t page, uint8_t reg, uint8_t *out);

/**
 * @brief Raw register write on either slave.
 *
 * @warning  Modifying CMI-controlled registers can interact with
 *           the power-sequence state machine in non-obvious ways.
 *           Prefer the high-level helpers; reach for raw writes only
 *           when implementing a feature the helpers don't cover and
 *           you have read the relevant datasheet section.
 *
 * @param ctx   ACT8760 context handle (must be initialised first).
 * @param page  Which slave address hosts the register.
 * @param reg   8-bit register address.
 * @param val   Byte to write.
 * @return      ALP_OK on success, ALP_ERR_NOT_READY if not initialised,
 *              ALP_ERR_INVAL on invalid args.
 */
alp_status_t act8760_write_reg(act8760_t *ctx, act8760_page_t page, uint8_t reg, uint8_t val);

/**
 * @brief Read a regulator's active `VSET0` register as a raw 7-bit
 *        (buck) or 6-bit (LDO) integer.
 *
 * Bits outside the VSET field (EN_OutPD / IPD_SET on bucks, RANGE on
 * LDOs) are masked off before returning @p vset_raw.  Caller maps
 * `vset_raw` -> mV using the rail's documented voltage range (see
 * ACT88760 Users Guide Rev 3.0, "Output Voltage Range" tables, or the
 * Per-rail voltage selection @par above).  This driver intentionally
 * does not encode the mapping -- a typed mV helper lands when a
 * consumer needs one (YAGNI).
 *
 * @param ctx       ACT8760 context handle (must be initialised first).
 * @param rail      Which regulator.
 * @param vset_raw  Raw VSET value (0..127 for bucks, 0..63 for LDOs).
 * @return          ALP_OK on success, ALP_ERR_NOT_READY if not
 *                  initialised, ALP_ERR_INVAL on NULL / invalid args.
 */
alp_status_t act8760_rail_get_vset(act8760_t *ctx, act8760_rail_t rail, uint8_t *vset_raw);

/**
 * @brief Write a regulator's active `VSET0` register.
 *
 * The new value applies immediately (volatile shadow).  Power-cycle
 * reverts to the CMI default.
 *
 * @note Read-modify-write: bits outside the VSET field (EN_OutPD /
 *       IPD_SET on bucks, RANGE on LDOs) are preserved.
 *
 * @param ctx       ACT8760 context handle (must be initialised first).
 * @param rail      Which regulator to program.
 * @param vset_raw  New VSET value.  Must fit in 7 bits for bucks,
 *                  6 bits for LDOs; otherwise ALP_ERR_INVAL.
 * @return          ALP_OK on success, ALP_ERR_NOT_READY if not
 *                  initialised, ALP_ERR_INVAL on invalid args.
 */
alp_status_t act8760_rail_set_vset(act8760_t *ctx, act8760_rail_t rail, uint8_t vset_raw);

/**
 * @brief Release resources.  Idempotent.
 *
 * @param ctx   ACT8760 context handle (may be NULL).
 */
void act8760_deinit(act8760_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_ACT8760_H */
