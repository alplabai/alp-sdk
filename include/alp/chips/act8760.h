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
 * bit (tile +6 bit1 on Buck1/2/7, tile +1 bit3 on Buck3/4 -- tile +6
 * bits1:0 are DBSTBY there; Buck5/6 are Output-High only).  LDO1/2: 6-bit VSET,
 * 12.5 mV step, 0.5-1.2875 V (RANGE=0) or 1.2-1.9875 V (RANGE=1).
 * LDO3-6: 0.5-1.2875 V (RANGE=0, 12.5 mV) or 1-4.15 V (RANGE=1, 50 mV).
 * The mV helpers decode / encode through the rail's LIVE range bit.
 * Buck1/2/7 additionally have VSET2/VSET3 DVS slots bank-aliased onto
 * the VSET0/1 addresses via MSTR 0x2C bit0 (BAND_SEL) -- out of this
 * driver's scope; while BAND_SEL is set their mV read / write returns
 * ::ALP_ERR_NOSUPPORT.
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
 *   This driver does NOT encode rail names, targets or guard windows:
 *   they live in the SoM family power tree
 *   (`metadata/e1m_modules/v2n/power-tree.yaml`) and reach the driver
 *   only through the limits table installed with act8760_set_limits().
 *
 * @par Guarded control (fail-closed)
 * Reads never need anything beyond act8760_init().  EVERY write --
 * rail voltage, rail enable/disable, GPIO polarity, raw register --
 * first needs a limits table installed with act8760_set_limits()
 * (generated from the SoM power tree, see <alp/chips/pmic_rail_limit.h>
 * and <alp/chips/v2n_power_tree.h>); without one it returns
 * ::ALP_ERR_NOSUPPORT.  With one installed:
 * - act8760_rail_set_voltage_mv() only programs a setpoint inside the
 *   rail's window, in the rail's LIVE range (it never flips a range bit);
 * - act8760_rail_set_enable() never disables a `critical` rail;
 * - act8760_gpio_set_polarity() only touches MODEx bit7, and only on a
 *   GPIO whose bit is set in the installed polarity-writable mask;
 * - act8760_write_reg() refuses (::ALP_ERR_NOSUPPORT) every register a
 *   typed API owns (each tile's status / VSET0..3 / ON / range registers,
 *   MSTR MODE1..11) and the hard-deny set: MSTR 0x00, 0x02..0x04, 0x06,
 *   0x07 (MR / SLEEP / DPSLP / POWER OFF / watchdog), 0x08 (factory
 *   password), 0x09, 0x0A, 0x14 (POK_OV / VSYSWARN thresholds -- a bad
 *   value can trip PMIC shutdown, deny until a typed API validates it),
 *   0x15..0x26, 0x2C, 0x2D..0x32, 0x34 and every tile's factory registers
 *   (buck tile +0x08..+0x1F, LDO dual tile +0x0B..+0x1F).  MSTR 0x0B /
 *   0x0C are denied too: they retime PMIC_RSTOUT / V2N_BOOT_CPU_SEL /
 *   DEEPX_PWR_EN_REQ, and 0x0C bits1:0 are the watchdog WDTIME / RETRY
 *   TIME.  Raw writes stay possible only to MSTR 0x01, 0x05, 0x2B, 0x33
 *   (IRQ masks, LED current) -- the `write: allow` rows of
 *   `metadata/chips/act8760.yaml`.
 * DVS (VSET1..VSET3, MSTR 0x2C) stays out of scope.
 *
 * @par Volatile vs non-volatile
 * I2C writes only update the volatile shadow.  Power-cycling reverts to
 * the CMI defaults -- the CMI stays the source of truth for the
 * production power sequence.
 */

#ifndef ALP_CHIPS_ACT8760_H
#define ALP_CHIPS_ACT8760_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"
#include "alp/chips/pmic_rail_limit.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Number of ACT88760 GPIOs (GPIO1..GPIO11); GPIO arguments are 1-based. */
#define ACT8760_GPIO_COUNT 11u

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

/** Decoded live state of one regulator tile. */
typedef struct {
	bool     enabled;    /**< Tile ON bit (enable register bit7). */
	uint8_t  vset_raw;   /**< VSET0 field, range bit masked off. */
	uint8_t  range;      /**< Live range (0/1; Buck5/6 report 1). */
	uint16_t voltage_mv; /**< VSET0 decoded through @ref range, rounded down; 0 if undecodable. */
	bool     pok;        /**< Status bit7: output in regulation. */
	bool     ov;         /**< Status bit6: over-voltage. */
	bool     ilim;       /**< Status bit5: current limit tripped. */
	bool     ilim_warn;  /**< Status bit4 (bucks only; false on LDOs). */
	uint8_t  status_raw; /**< Untouched tile status byte. */
} act8760_rail_state_t;

/** Decoded state of one ACT88760 GPIO. */
typedef struct {
	bool    level;      /**< Real-time pin status (MSTR 0x03 / 0x2B). */
	bool    inverted;   /**< MODEx bit7: polarity inverted. */
	uint8_t mux;        /**< MODEx bits3:0: function MUX (factory-set). */
	uint8_t mode_raw;   /**< Untouched MODEx byte (e.g. 0x88 = GPIO4 OTP defect). */
	bool    push_pull;  /**< MSTR 0x34 push-pull enable (false for GPIO5, GPIO9..11: no bit). */
	bool    irq_masked; /**< Input-toggle IRQ mask (MSTR 0x05 / 0x2B / 0x2C bit1). */
} act8760_gpio_state_t;

/** Driver context.  Fields are driver-private; use the API. */
typedef struct {
	bool                     initialised; /**< Set by act8760_init(). */
	alp_i2c_t               *bus;         /**< BRD_I2C handle. */
	uint8_t                  addr_page0;  /**< Defaults to ACT8760_I2C_ADDR_PAGE0. */
	uint8_t                  addr_page1;  /**< Defaults to ACT8760_I2C_ADDR_PAGE1. */
	const pmic_rail_limit_t *limits;      /**< ACT8760_RAIL_COUNT entries, or NULL = fail-closed. */
	uint16_t                 gpio_polarity_ok; /**< Bit (n-1) set = GPIOn polarity writable. */
	uint16_t                 gpio_toggles;     /**< Harvested toggle bits, bit (n-1) = GPIOn. */
} act8760_t;

/**
 * @brief Probe both slaves of the PMIC and initialise the context.
 *
 * Reads register 0x03 on the ADD1 slave (GPIO levels -- not 0x00, whose
 * VSYS latches clear on read) and register
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
 * @brief Raw register write on either slave -- deny-listed.
 *
 * Only the `write: allow` registers of `metadata/chips/act8760.yaml`
 * (ADD1 0x01, 0x05, 0x2B, 0x33) are ever written.
 * Every register a typed API owns, the power-state / factory set (MSTR
 * 0x07, 0x08, 0x09, 0x0A, 0x14, 0x15..0x26, 0x2D..0x32, ...) and every
 * unlisted address are refused -- see the file-level "Guarded control"
 * section for the full set.
 *
 * @param ctx   ACT8760 context handle (must be initialised first).
 * @param page  Which slave address hosts the register.
 * @param reg   8-bit register address.
 * @param val   Byte to write.
 * @return      ALP_OK on success; ALP_ERR_NOT_READY if not initialised;
 *              ALP_ERR_INVAL on an invalid page; ALP_ERR_NOSUPPORT if no
 *              limits table is installed or @p reg is not raw-writable;
 *              the bus status on an I2C failure.
 */
alp_status_t act8760_write_reg(act8760_t *ctx, act8760_page_t page, uint8_t reg, uint8_t val);

/**
 * @brief Install the guard table that unlocks every control write.
 *
 * The driver stores the pointer only; @p rails must outlive @p ctx (use
 * static storage initialised from the generated
 * `V2N_POWER_ACT8760_RAIL_LIMITS_INIT`).  Call after act8760_init() --
 * init clears it.  Passing @p rails == NULL uninstalls the table (back to
 * fail-closed).
 *
 * @param ctx                      ACT8760 context handle (initialised).
 * @param rails                    ACT8760_RAIL_COUNT entries indexed by
 *                                 ::act8760_rail_t, or NULL.
 * @param gpio_polarity_writable   Bit (n-1) set = GPIOn's MODEx polarity
 *                                 bit may be written (generated
 *                                 `V2N_POWER_ACT8760_GPIO_POLARITY_WRITABLE_MASK`).
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL if
 *         a voltage-writable entry has min_mv > max_mv or a mask bit above
 *         GPIO11 is set.
 */
alp_status_t
act8760_set_limits(act8760_t *ctx, const pmic_rail_limit_t *rails, uint16_t gpio_polarity_writable);

/**
 * @brief Read one regulator tile's live state (enable, setpoint, faults).
 *
 * Reads the tile's status, VSET0, ON and range registers; decodes the
 * setpoint through the live range bit.  For LDO5/LDO6 configured as load
 * switches @p out->voltage_mv is the (meaningless) VSET decode.
 *
 * @param ctx   ACT8760 context handle (must be initialised first).
 * @param rail  Which regulator.
 * @param out   Receives the decoded state.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL on
 *         NULL @p out or an invalid rail; the bus status on I2C failure.
 */
alp_status_t act8760_rail_get_state(act8760_t *ctx, act8760_rail_t rail, act8760_rail_state_t *out);

/**
 * @brief Read a rail's programmed VSET0 setpoint in millivolts.
 *
 * @param ctx   ACT8760 context handle (must be initialised first).
 * @param rail  Which regulator.
 * @param mv    Receives the decoded setpoint (live range, rounded down).
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL on
 *         NULL @p mv or an invalid rail; ALP_ERR_NOSUPPORT when BAND_SEL aliases VSET0 to
 *         VSET2 (Buck1/2/7); the bus status on I2C failure.
 */
alp_status_t act8760_rail_get_voltage_mv(act8760_t *ctx, act8760_rail_t rail, uint16_t *mv);

/**
 * @brief Program a rail's VSET0 setpoint -- window-guarded.
 *
 * Encodes @p mv in the rail's LIVE range (read back first; the range bit
 * is never changed), rounding down to the VSET grid, then refuses unless
 * the ENCODED value is inside the installed window.  Read-modify-write:
 * EN_OutPD / IPD_SET / RANGE bits sharing the byte are preserved.
 *
 * @param ctx   ACT8760 context handle (must be initialised first).
 * @param rail  Which regulator.
 * @param mv    Requested setpoint.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL on
 *         an invalid rail; ALP_ERR_NOSUPPORT if no table is installed,
 *         the rail is not voltage-writable or BAND_SEL is set (Buck1/2/7);
 *         ALP_ERR_OUT_OF_RANGE if the encoded setpoint falls outside
 *         [min_mv, max_mv] or the live range's code space; ALP_ERR_IO if
 *         the read-back differs; the bus status on I2C failure.
 */
alp_status_t act8760_rail_set_voltage_mv(act8760_t *ctx, act8760_rail_t rail, uint16_t mv);

/**
 * @brief Set or clear a rail's tile ON bit -- critical rails never go off.
 *
 * @param ctx     ACT8760 context handle (must be initialised first).
 * @param rail    Which regulator.
 * @param enable  true = ON, false = OFF.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL on
 *         an invalid rail; ALP_ERR_NOSUPPORT if no table is installed, the
 *         rail is not enable-writable, or @p enable is false on a
 *         `critical` rail; ALP_ERR_IO if the read-back differs; the bus
 *         status on I2C failure.
 */
alp_status_t act8760_rail_set_enable(act8760_t *ctx, act8760_rail_t rail, bool enable);

/**
 * @brief Read one GPIO's level, polarity, MUX, drive type and IRQ mask.
 *
 * Reading GPIO9..GPIO11 reads MSTR 0x2B, whose toggle bits clear on read;
 * any toggle bits seen are OR-ed into the context latch so
 * act8760_gpio_toggles_peek() still reports them.
 *
 * @param ctx   ACT8760 context handle (must be initialised first).
 * @param gpio  1..ACT8760_GPIO_COUNT.
 * @param out   Receives the decoded state.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL on
 *         NULL @p out or @p gpio out of range; the bus status on I2C failure.
 */
alp_status_t act8760_gpio_get(act8760_t *ctx, uint8_t gpio, act8760_gpio_state_t *out);

/**
 * @brief Report GPIO toggle events without discarding them.
 *
 * The chip clears its toggle bits (MSTR 0x04, 0x2B) on read; this call
 * harvests them into the context latch and returns the latch, leaving it
 * intact.
 *
 * @param ctx   ACT8760 context handle (must be initialised first).
 * @param mask  Receives the latch, bit (n-1) = GPIOn toggled.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL on
 *         NULL @p mask; the bus status on I2C failure.
 */
alp_status_t act8760_gpio_toggles_peek(act8760_t *ctx, uint16_t *mask);

/**
 * @brief Report GPIO toggle events and clear the context latch.
 *
 * Same harvest as act8760_gpio_toggles_peek(), then zeroes the latch.
 *
 * @param ctx   ACT8760 context handle (must be initialised first).
 * @param mask  Receives the latch before clearing, bit (n-1) = GPIOn.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL on
 *         NULL @p mask; the bus status on I2C failure.
 */
alp_status_t act8760_gpio_toggles_clear(act8760_t *ctx, uint16_t *mask);

/**
 * @brief Write one GPIO's MODEx polarity bit (bit7) -- mask-guarded.
 *
 * Read-modify-write of MODEx touching bit7 only (MUX / factory bits are
 * preserved), then read back.  The V2N use: clearing the inverted-OTP
 * polarity on GPIO4 (GD32_NRST, MODE4 0x10 reads 0x88 -> 0x08); the write
 * is volatile and reverts at the next power cycle.
 *
 * @param ctx       ACT8760 context handle (must be initialised first).
 * @param gpio      1..ACT8760_GPIO_COUNT.
 * @param inverted  New polarity bit value.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL on
 *         @p gpio out of range; ALP_ERR_NOSUPPORT if no table is installed
 *         or @p gpio is not in the polarity-writable mask; ALP_ERR_IO if
 *         the read-back differs; the bus status on I2C failure.
 */
alp_status_t act8760_gpio_set_polarity(act8760_t *ctx, uint8_t gpio, bool inverted);

/**
 * @brief Read a regulator's active `VSET0` register as a raw 7-bit
 *        (buck) or 6-bit (LDO) integer.
 *
 * Bits outside the VSET field (EN_OutPD / IPD_SET on bucks, RANGE on
 * LDOs) are masked off before returning @p vset_raw.  For a decoded
 * setpoint use act8760_rail_get_voltage_mv().
 *
 * @param ctx       ACT8760 context handle (must be initialised first).
 * @param rail      Which regulator.
 * @param vset_raw  Raw VSET value (0..127 for bucks, 0..63 for LDOs).
 * @return          ALP_OK on success, ALP_ERR_NOT_READY if not
 *                  initialised, ALP_ERR_INVAL on NULL / invalid args.
 */
alp_status_t act8760_rail_get_vset(act8760_t *ctx, act8760_rail_t rail, uint8_t *vset_raw);

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
