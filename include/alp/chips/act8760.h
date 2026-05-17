/*
 * Copyright 2026 ALP Lab AB
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
 * - 7x DC/DC step-down bucks (Buck1..Buck6 on page-0, Buck7 on page-1)
 * - 6x LDOs (LDO1..LDO6 on page-1; LDO5/6 can be configured as
 *   load-switches)
 * - 11x configurable GPIOs (GPIO1..GPIO11) on page-0
 * - VIO reference (1.62..1.8 V), nIRQ, nRESET, nPB, PWREN/PWRON pins
 *
 * @par I2C slave addressing
 * The chip presents **two adjacent 7-bit slave addresses** that the
 * V2N schematic populates as `0x25` (page-0, system + Buck1..Buck6
 * + GPIOs) and `0x26` (page-1, Buck7 + LDO1..LDO6).  Both addresses
 * live on the same physical bus (Renesas RIIC8 / BRD_I2C, master
 * pads `P07` / `P06`).
 *
 * Other CMI variants of the chip use the address pairs
 * `0x27/0x28`, `0x67/0x68`, `0x6B/0x6C` -- the V2N-specific 120.E1
 * variant uses `0x25/0x26` and the driver hard-codes that pair via
 * the `ACT8760_I2C_ADDR_PAGE0` / `_PAGE1` macros.
 *
 * @par Per-rail voltage selection
 * Each buck regulates to a voltage selected by its `Bx_VSETy` register
 * (7-bit unsigned).  Buck1/2/7 have four `VSETy` (y=0..3 for DVS);
 * Buck3..Buck6 have two (`VSET0` for normal, `VSET1` for SLEEP/DPSLP).
 * LDOs have a single 6-bit `VSET` register.  The exact VSET-to-mV
 * mapping depends on the regulator's voltage range and is documented
 * in the ACT88760 Users Guide (Qorvo doc, Rev 3.0) -- it is NOT a
 * single closed-form formula, so this driver exposes the raw VSET
 * value and leaves mV conversion to a higher layer that knows the
 * rail's configured range.
 *
 * @par Status surface
 * - Power-on faults (UV/OV/ILIM per regulator) latched to fault-flag
 *   registers; cleared by an I2C read after the fault clears.
 * - Thermal warning bit `TWARN` (datasheet section "Thermal"): the
 *   IC asserts `nIRQ` and (unless masked) shuts down when the
 *   junction crosses 165 degC.  This driver exposes `act8760_get_status`
 *   to read the system-status word at register `0x00` and surface
 *   the TWARN / SYSDAT / SYSWARN / ILIM bits to the application.
 *
 * @par Datasheet provenance
 * - **ACT88760 Datasheet Rev C, August 5, 2025** (Qorvo / Active-Semi,
 *   NDA-protected) -- block diagram + register-naming conventions.
 * - **ACT88760 Users Guide Rev 3.0** -- VSET-to-mV tables per range.
 * - **ACT88760 ActiveCiPS Programming Guide Rev 0.1** -- CMI flow.
 * - **ACT88760_CMI120 Power Sequence_250916.pdf** -- the V2N power-on
 *   timing diagram + per-rail target voltage list for CMI 120.E1.
 *   This driver does NOT encode the rail names from that PDF in code
 *   -- carrier-board firmware reads them from `metadata/chips/act8760.yaml`
 *   instead (the rail manifest), so a CMI swap doesn't require a code
 *   rebuild.
 *
 * @par Operating mode
 * - During normal operation the host's role is **read-only telemetry +
 *   error reporting**: poll `act8760_get_status`, react on faults,
 *   read VSET back to confirm a rail came up to its programmed value.
 * - DVS (dynamic voltage scaling) is supported via `act8760_buck_set_vset`
 *   on Buck1/2/7's VSET1/2/3 slots; the change takes effect when a
 *   configured GPIO asserts or when the host writes the `BkxDvsI2C`
 *   bits in register `0x2C` (page-0).
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

/** Page-0 I2C 7-bit slave address for CMI 120.E1 (system + Buck1..6
 *  + GPIOs).  Matches the V2N schematic. */
#define ACT8760_I2C_ADDR_PAGE0 0x25u

/** Page-1 I2C 7-bit slave address for CMI 120.E1 (Buck7 + LDO1..6).
 *  Lives at PAGE0+1 by chip design. */
#define ACT8760_I2C_ADDR_PAGE1 0x26u

/** Page selector for raw register access. */
typedef enum {
    ACT8760_PAGE_SYSTEM = 0, /**< System + Buck1..Buck6 + GPIOs (0x25). */
    ACT8760_PAGE_AUX    = 1, /**< Buck7 + LDO1..LDO6 (0x26). */
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

/** Decoded system status (register 0x00, page-0). */
typedef struct {
    bool thermal_warning; /**< TWARN: junction >= warning threshold (default 165 degC). */
    bool sys_warning;     /**< SYSWARN: AVIN below the SYSWARN voltage. */
    bool sys_data;        /**< SYSDAT: AVIN below SYSMON (lower than SYSWARN). */
    bool ilim_warning;    /**< Any regulator hit current-limit warning. */
    bool fault_pending;   /**< Any unmasked fault flag latched. */
    uint8_t raw;          /**< Untouched register byte for diagnostic logging. */
} act8760_status_t;

/** Driver context. */
typedef struct {
    bool       initialised;
    alp_i2c_t *bus;
    uint8_t    addr_page0; /**< Defaults to ACT8760_I2C_ADDR_PAGE0. */
    uint8_t    addr_page1; /**< Defaults to ACT8760_I2C_ADDR_PAGE1. */
} act8760_t;

/**
 * @brief Probe both pages of the PMIC and initialise the context.
 *
 * Issues a zero-length write to each of `addr_page0` / `addr_page1`
 * to confirm the IC ACKs at the expected pair.  Does **not** alter
 * the volatile shadow registers -- the IC starts up in the CMI
 * default state and the host is expected to leave power-tree config
 * alone for normal operation.
 *
 * @param ctx   Driver context (output).
 * @param bus   I2C bus handle for BRD_I2C.
 * @return      ALP_OK on success, ALP_ERR_NOT_READY if either page
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
 * @param addr_page0  Page-0 7-bit address.  Page-1 is assumed to be
 *                    at `addr_page0 + 1` per chip design.
 */
alp_status_t act8760_init_at(act8760_t *ctx, alp_i2c_t *bus, uint8_t addr_page0);

/**
 * @brief Read the system-status register (0x00, page-0) and decode
 *        the most useful bits.
 *
 * @note  Reading the register **also clears** the latched nIRQ bits
 *        per the datasheet's "I2C read clears IRQ" semantics.  Call
 *        this periodically or in response to a nIRQ assertion.
 */
alp_status_t act8760_get_status(act8760_t *ctx, act8760_status_t *out);

/**
 * @brief Raw register read on either page.
 *
 * @param ctx   ACT8760 context handle (must be initialised first).
 * @param page  Which slave-address page hosts the register.
 * @param reg   8-bit register address.
 * @param out   Pointer to the destination byte.
 */
alp_status_t act8760_read_reg(act8760_t *ctx, act8760_page_t page,
                              uint8_t reg, uint8_t *out);

/**
 * @brief Raw register write on either page.
 *
 * @warning  Modifying CMI-controlled registers can interact with
 *           the power-sequence state machine in non-obvious ways.
 *           Prefer the high-level helpers; reach for raw writes only
 *           when implementing a feature the helpers don't cover and
 *           you have read the relevant datasheet section.
 */
alp_status_t act8760_write_reg(act8760_t *ctx, act8760_page_t page,
                               uint8_t reg, uint8_t val);

/**
 * @brief Read a regulator's active `VSET0` register as a raw 7-bit
 *        (buck) or 6-bit (LDO) integer.
 *
 * Caller maps `vset_raw` -> mV using the rail's documented voltage
 * range (see ACT88760 Users Guide Rev 3.0, "Output Voltage Range"
 * tables).  This driver intentionally does not encode the mapping
 * since the per-range table is rail- and CMI-dependent.
 *
 * @param ctx       ACT8760 context handle (must be initialised first).
 * @param rail      Which regulator.
 * @param vset_raw  Raw VSET value (0..127 for bucks, 0..63 for LDOs).
 */
alp_status_t act8760_rail_get_vset(act8760_t *ctx, act8760_rail_t rail,
                                   uint8_t *vset_raw);

/**
 * @brief Write a regulator's active `VSET0` register.
 *
 * The new value applies immediately (volatile shadow).  Power-cycle
 * reverts to the CMI default.
 *
 * @param ctx       ACT8760 context handle (must be initialised first).
 * @param rail      Which regulator to program.
 * @param vset_raw  New VSET value.  Must fit in 7 bits for bucks,
 *                  6 bits for LDOs; otherwise ALP_ERR_INVAL.
 */
alp_status_t act8760_rail_set_vset(act8760_t *ctx, act8760_rail_t rail,
                                   uint8_t vset_raw);

/** @brief Release resources.  Idempotent. */
void         act8760_deinit(act8760_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_ACT8760_H */
