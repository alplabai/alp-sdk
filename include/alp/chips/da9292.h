/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file da9292.h
 * @brief Renesas DA9292 high-current multi-phase buck PMIC driver.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * @par Verification status: [REGISTER-MAP VERIFIED]
 *   `PMC_STATUS_00` bit layout verified 2026-06-06 against DA9292
 *   Datasheet Rev 2.2 (R16DS0518EJ0220), Table 14 (p.36-37):
 *   bits[7:0] = S_CH2_OC, S_CH1_OC, S_CH2_OV, S_CH1_OV,
 *               S_CH2_UV, S_CH1_UV, S_CH2_PG, S_CH1_PG.
 *   The assumption that STATUS_00 mirrors MASK_00 (Table 18) was
 *   correct.  The [UNTESTED] on-silicon caveat below remains -- no
 *   HiL bring-up yet; validation happens on the patched BRD_I2C bus
 *   via `examples/v2n/v2n-brd-i2c-bringup`.
 *
 * The DA9292 is a multi-phase DC-DC buck PMIC that can be configured
 * (via the silicon's `CONF` strap pin) as either:
 *
 *   - **1x quad-phase converter** (up to 52 A peak), or
 *   - **2x dual-phase converters** (10 A each, 26 A peak per
 *     channel).
 *
 * On the V2N / V2N-M1 SoMs the chip is strapped for **2-channel
 * dual-phase mode** (CONF strap value selected for that config) and
 * sits on the BRD_I2C bus at 7-bit address `0x1E` (8-bit `0x3C` for
 * write, `0x3D` for read) -- matches the DA9292-AROVx OTP variant
 * register `PMC_CFG_0A` reset value of `0x3C`.
 *
 * @par Channel-to-rail mapping on V2N
 *
 * Two channels, each backed by two phases:
 *
 *   - **CH1** (phases 1 + 2) -> 0.8 V Renesas rail.  Enabled at
 *     boot by the board-driven `EN1` strap; firmware doesn't have
 *     to touch it.
 *   - **CH2** (phases 3 + 4) -> 0.75 V DEEPX DDR / NPU rail.
 *     **Disabled at boot on every variant**; only V2N-M1 firmware
 *     programs CH2's `CH2_VOUT_VSEL_LO` to 0.75 V and then writes
 *     `CH2_EN = 1` (in register `PMC_CTRL_01` bit 1) before
 *     deasserting `M1_RESET`.  On V2N base CH2 stays disabled
 *     because DEEPX isn't populated.
 *
 * The phase pairs themselves don't surface as separate channels at
 * the I2C register level -- callers see only CH1 and CH2.  The
 * V2N + V2N-M1 SoM presets (`metadata/e1m_modules/E1M-V2{N,M}*.yaml`)
 * use "ch1+ch2 / ch3+ch4" notation;
 * that refers to the underlying phase pairs that make up each
 * channel (phases 1+2 = CH1, phases 3+4 = CH2).
 *
 * @par Output voltage encoding
 *
 * Two voltage ranges per channel, selected by `CHx_VSTEP`:
 *
 *   - **VSTEP = 0 (default)**: 0.3..1.275 V in 5 mV steps.  Register
 *     byte = `0x3C + (mV - 300) / 5`.  Codes `0x00..0x3B` reserved.
 *   - **VSTEP = 1**: 0.6..1.9 V in 10 mV steps (full doubled range).
 *
 * Both V2N targets (0.8 V, 0.75 V) fit comfortably in VSTEP=0 so
 * the driver hard-codes that range and rejects `_set_voltage_mv`
 * requests outside `[300, 1275]`.  Higher-range work requires a
 * `_set_vstep` extension that disables the channel first (per
 * datasheet: "The buck converter needs to be disabled (CHx_EN = 0)
 * before CHx_VSTEP setting can be changed by I2C write").
 *
 * @warning The DA9292-AROVx OTP variant used on V2N boots with
 *          `CH2_VSTEP=1` (PMC_CTRL_01 reset value 0x80).
 *          `_set_voltage_mv` does NOT touch VSTEP, so callers that
 *          intend to use the 5 mV / 0.3-1.275 V range on CH2 must
 *          clear `CH2_VSTEP` (while `CH2_EN=0`) first --
 *          `da9292_v2n_m1_enable_deepx_rail` does this internally.
 *
 * @par Power-good and event handling
 *
 * Both channels assert a per-channel power-good (`CHx_PG`) status
 * bit when their output rises above `VTHR_UV_RISE` and lose it when
 * the output drops below `VTHR_UV_FALL`.  Event flags (`E_CHx_UV` /
 * `E_CHx_OV` / `E_TEMP_WARN` / `E_TEMP_CRIT` / `E_VIN_UVLO`) latch
 * in `PMC_EVENT_00` / `PMC_EVENT_01` and assert the `INT_N` line;
 * clear them by writing `1` to the corresponding bit.  This driver
 * surfaces both the live status (via `da9292_get_status`) and the
 * latched events (via `da9292_read_and_clear_events`).
 *
 * @par Board-side IRQ wiring
 *
 * On V2N the two IO outputs from DA9292 are routed to the Renesas
 * RZ/V2N (after the 2026-05-11 schematic revision that reassigned
 * `P36` / `P37` away from `GPT15_GTIOC15A/B`):
 *
 *   - `TW_N` -> Renesas `P36` (`DA9292_TW`) -- thermal-warning low.
 *   - `INT_N` -> Renesas `P37` (`DA9292_INT`) -- generic interrupt
 *     output that goes low on any unmasked event.
 *
 * The driver itself doesn't grab those GPIOs -- board-side code opens
 * them and either wires the `alp_gpio_*` ISR to call
 * `da9292_read_and_clear_events` when the line falls, or polls the
 * packed level snapshot via `da9292_get_fault_pins`.  (The GD32
 * bridge's `DA9292_STATUS_FORWARD` opcode answers the `0xFF` sentinel
 * on this SoM revision -- no DA9292 net reaches the GD32 -- so the
 * direct read here is the working fault-pin path.)
 *
 * @par Datasheet provenance
 * - **REN_DA9292_Datasheet_2v2_DST_20250323.pdf** (Renesas) -- full
 *   register map (Tables 12-41), I2C protocol, voltage encoding.
 * - **DA9292-AROVx Variant Overview_01v00.pdf** -- variant matrix.
 * - **REN_AN-PM-189_DA9292_PCB_Layout_Recommendations_Rev2.pdf** --
 *   layout guidance (not register-relevant; archived for board work).
 */

#ifndef ALP_CHIPS_DA9292_H
#define ALP_CHIPS_DA9292_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 7-bit I2C slave address used on V2N / V2N-M1.  The chip's
 *  `PMC_CFG_0A` register holds the address byte (`0x3C` 8-bit
 *  write-form for the AROVx OTP variant our SoMs use); the 7-bit
 *  bus address is `0x1E`. */
#define DA9292_I2C_ADDR_V2N 0x1Eu

/** Channel identifier (the two dual-phase output channels in V2N's
 *  CONF configuration). */
typedef enum {
	DA9292_CH1 = 0, /**< Phases 1 + 2 -- 0.8 V Renesas rail on V2N. */
	DA9292_CH2 = 1, /**< Phases 3 + 4 -- 0.75 V DEEPX rail on V2N-M1. */
} da9292_channel_t;

/** Decoded `PMC_STATUS_00` + `PMC_STATUS_01` snapshot. */
typedef struct {
	bool    ch1_pg;    /**< CH1 power-good (output in regulation). */
	bool    ch2_pg;    /**< CH2 power-good. */
	bool    ch1_uv;    /**< CH1 under-voltage (live). */
	bool    ch2_uv;    /**< CH2 under-voltage. */
	bool    ch1_ov;    /**< CH1 over-voltage. */
	bool    ch2_ov;    /**< CH2 over-voltage. */
	bool    ch1_oc;    /**< CH1 over-current (live). */
	bool    ch2_oc;    /**< CH2 over-current. */
	bool    temp_warn; /**< Thermal warning threshold crossed. */
	bool    temp_crit; /**< Thermal critical threshold (shutdown imminent). */
	bool    vin_uvlo;  /**< Input UVLO -- supply below operating range. */
	uint8_t raw_00;    /**< Untouched PMC_STATUS_00 byte for diagnostics. */
	uint8_t raw_01;    /**< Untouched PMC_STATUS_01 byte. */
} da9292_status_t;

/** @brief Latched-event snapshot (read-and-clear from `PMC_EVENT_00/01`).
 *
 *  Each flag is true if that event latched since the last read; the read
 *  clears it on the chip (write-1-to-clear). */
typedef struct {
	bool e_ch1_pg;    /**< CH1 power-good transition latched. */
	bool e_ch2_pg;    /**< CH2 power-good transition latched. */
	bool e_ch1_uv;    /**< CH1 under-voltage event latched. */
	bool e_ch2_uv;    /**< CH2 under-voltage event latched. */
	bool e_ch1_ov;    /**< CH1 over-voltage event latched. */
	bool e_ch2_ov;    /**< CH2 over-voltage event latched. */
	bool e_ch1_oc;    /**< CH1 over-current event latched. */
	bool e_ch2_oc;    /**< CH2 over-current event latched. */
	bool e_temp_warn; /**< Thermal-warning event latched. */
	bool e_temp_crit; /**< Thermal-critical event latched. */
	bool e_vin_uvlo;  /**< Input UVLO event latched. */
} da9292_events_t;

/** @brief Driver context.  Caller-allocated; fields are driver-private. */
typedef struct {
	bool       initialised; /**< True between a successful init and deinit. */
	alp_i2c_t *bus;         /**< Borrowed BRD_I2C handle; caller retains ownership. */
	uint8_t    addr;        /**< 7-bit I2C slave address. */
	uint8_t    dev_id;      /**< Cached PMC_DEV_ID (read at init). */
	uint8_t    rev_id;      /**< Cached PMC_REV_ID. */
} da9292_t;

/** @brief Probe + cache device + revision identifiers.
 *
 *  @note  Does not change the volatile register state -- the chip's
 *         OTP carries the V2N power-on defaults, and per V2N
 *         schematic the EN1 pin is strapped so CH1 enables itself
 *         at VSYS rise.  Touching CH1 from firmware would risk
 *         glitching the 0.8 V Renesas rail.
 *
 *  @param ctx        Driver context (output).
 *  @param bus        Opened BRD_I2C handle.
 *  @param addr_7bit  7-bit slave address (0x1E on V2N -- @ref DA9292_I2C_ADDR_V2N).
 *  @return ALP_OK with identifiers cached; ALP_ERR_INVAL on a NULL argument;
 *          otherwise the underlying I2C status. */
alp_status_t da9292_init(da9292_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit);

/** @brief Read both status bytes and decode the bit fields.
 *
 *  @param ctx  Initialised driver context.
 *  @param out  Receives the decoded live-status snapshot.
 *  @return ALP_OK with @p out filled; ALP_ERR_INVAL on a NULL argument;
 *          otherwise the underlying I2C status. */
alp_status_t da9292_get_status(da9292_t *ctx, da9292_status_t *out);

/** @brief Read + clear `PMC_EVENT_00/01` (write-1-to-clear semantics).
 *
 *  @param ctx  Initialised driver context.
 *  @param out  Receives the latched-event snapshot taken before the clear.
 *  @return ALP_OK with @p out filled and the events cleared on-chip;
 *          ALP_ERR_INVAL on a NULL argument; otherwise the I2C status. */
alp_status_t da9292_read_and_clear_events(da9292_t *ctx, da9292_events_t *out);

/** @brief Sample the DA9292 fault pins and pack them into a flag byte.
 *
 *  Reads two already-opened GPIO inputs wired to the chip's
 *  open-drain, active-low fault outputs -- on V2N: `INT_N` = Renesas
 *  `P37` (`DA9292_INT`), `TW_N` = Renesas `P36` (`DA9292_TW`) -- and
 *  packs:
 *    - bit0 = `INT_N` asserted (pin reads low)
 *    - bit1 = `TW_N`  asserted (pin reads low)
 *    - bits 2-7 = 0
 *
 *  The packing matches the GD32 bridge's `DA9292_STATUS_FORWARD`
 *  (opcode `0x40`) reply byte, so code written against that contract
 *  works unchanged if a future SoM revision lets the bridge serve it.
 *  On the current revision the bridge answers `0xFF` ("no sample") --
 *  this direct read is the working fault-pin path.
 *
 *  @param int_n  Opened input on the `DA9292_INT` net, or NULL to
 *                skip (bit0 then reports deasserted).
 *  @param tw_n   Opened input on the `DA9292_TW` net, or NULL to
 *                skip (bit1 then reports deasserted).
 *  @param flags  Receives the packed byte.  Required.
 *  @return ALP_OK, ALP_ERR_INVAL on NULL @p flags, or the first
 *          failing `alp_gpio_read` status. */
alp_status_t da9292_get_fault_pins(alp_gpio_t *int_n, alp_gpio_t *tw_n, uint8_t *flags);

/** @brief Enable (or disable) a channel via `CHx_EN` in `PMC_CTRL_01`.
 *
 *  @note  This sets/clears the **register-side** enable.  When the
 *         CONF strap routes EN1/EN2 to host GPIOs the channel is
 *         the AND of pin and register; when the board ties EN1/EN2
 *         to AVDD (always-on), only the register matters.  The V2N
 *         schematic ties EN1 always-on and routes EN2 to a host
 *         signal -- check the V2N peripheral map for the exact
 *         board wiring.
 *
 *  @param ctx     Initialised driver context.
 *  @param ch      Channel to enable/disable.
 *  @param enable  true sets `CHx_EN`, false clears it.
 *  @return ALP_OK on success; ALP_ERR_INVAL on a NULL/bad-channel argument;
 *          otherwise the underlying I2C status. */
alp_status_t da9292_set_enable(da9292_t *ctx, da9292_channel_t ch, bool enable);

/** @brief Set channel output voltage in millivolts (VSTEP = 0, 5 mV step).
 *
 *  Writes the channel's `CHx_VOUT_VSEL_LO` register (`0x0A` for CH1,
 *  `0x0C` for CH2).  Pre-conditions:
 *    - `mv` in `[300, 1275]`; outside that returns `ALP_ERR_INVAL`.
 *    - `mv` must be a multiple of 5; rounded down otherwise.
 *
 *  @warning  Don't ramp by big steps -- DA9292 detects instantaneous
 *            over/under-voltage if the target jumps by more than ~50 mV
 *            in one write.  Step in 50 mV increments for big swings.
 *
 *  @param ctx  Initialised driver context.
 *  @param ch   Channel to program.
 *  @param mv   Target output in mV; must be in [300, 1275] (rounded down to a
 *              5 mV step).  Does NOT touch `CHx_VSTEP` (see @warning above).
 *  @return ALP_OK on success; ALP_ERR_INVAL on a NULL/bad-channel argument or
 *          an out-of-range @p mv; otherwise the underlying I2C status. */
alp_status_t da9292_set_voltage_mv(da9292_t *ctx, da9292_channel_t ch, uint16_t mv);

/** @brief Read back the channel's `CHx_VOUT_VSEL_LO` setpoint in mV.
 *
 *  @param ctx  Initialised driver context.
 *  @param ch   Channel to query.
 *  @param mv   Receives the decoded setpoint in mV (VSTEP=0 / 5 mV step).
 *  @return ALP_OK with @p mv set; ALP_ERR_INVAL on a NULL/bad-channel
 *          argument; otherwise the underlying I2C status. */
alp_status_t da9292_get_voltage_mv(da9292_t *ctx, da9292_channel_t ch, uint16_t *mv);

/**
 * @brief V2N base boot-time init.  Confirms CH2 is disabled (the
 *        chip's reset default already does this; the call is a
 *        belt-and-braces sanity check + a clear logging surface
 *        for production-test).
 *
 *  Safe to call on V2N-M1 -- it leaves CH2 untouched if `ctx->dev_id`
 *  was probed successfully and CH2 was already enabled by the
 *  M1-specific init path.
 *
 *  @param ctx  Initialised driver context.
 *  @return ALP_OK if CH2 is confirmed disabled; ALP_ERR_INVAL on a NULL
 *          context; otherwise the underlying I2C status.
 */
alp_status_t da9292_v2n_base_init(da9292_t *ctx);

/**
 * @brief V2N-M1 DEEPX rail bring-up sequence (CH2 -> 0.75 V).
 *
 *  Procedure:
 *
 *    1. Force `CH2_VSTEP = 0` and `CH2_EN = 0` in `PMC_CTRL_01`.
 *       The DA9292-AROVx OTP variant on V2N boots with
 *       `PMC_CTRL_01 = 0x80` (CH2_VSTEP=1, doubled-range), so the
 *       VSTEP=0 byte for 0.75 V (0x96) would otherwise be decoded
 *       as 1.50 V.  VSTEP is only writable while the channel is
 *       disabled, hence the EN-clear in the same write.
 *    2. Write `CH2_VOUT_VSEL_LO = 0.75 V` (0x96 at VSTEP=0).
 *    3. Read back, confirm the write took.
 *    4. Write `CH2_EN = 1` in `PMC_CTRL_01`.  On the V2N board
 *       the EN2 pin is wired to V2N GPIO P64 (`DEEPX_CORE_0P75_EN`);
 *       the recommended bring-up has firmware drive that pin
 *       high after this call, so the register-side enable here is
 *       belt-and-braces.
 *    5. Poll `da9292_get_status` for `ch2_pg = true` up to a
 *       caller-supplied timeout (typically <= 5 ms per datasheet
 *       soft-start figures).
 *
 *  Pre-condition: `da9292_v2n_base_init` has already run (CH2 is
 *  disabled).  This function re-asserts CH2_EN=0 defensively
 *  before touching VSTEP so that callers who skipped base_init
 *  still get a safe ordering.
 *
 *  @param ctx         Initialised driver context.
 *  @param timeout_us  Upper bound (microseconds) on the CH2 power-good poll.
 *  @return ALP_OK if CH2 reaches PG within `timeout_us`,
 *          ALP_ERR_TIMEOUT otherwise.
 */
alp_status_t da9292_v2n_m1_enable_deepx_rail(da9292_t *ctx, uint32_t timeout_us);

/** Raw register R/W (for diagnostics / advanced use). */
/** @brief Raw register read for diagnostics / advanced use.
 *  @param ctx  Initialised driver context.
 *  @param reg  Register byte offset.
 *  @param val  Receives the register contents.
 *  @return ALP_OK with @p val set; ALP_ERR_INVAL on a NULL argument; otherwise
 *          the underlying I2C status. */
alp_status_t da9292_read_reg(da9292_t *ctx, uint8_t reg, uint8_t *val);
/** @brief Raw register write for diagnostics / advanced use.
 *  @param ctx  Initialised driver context.
 *  @param reg  Register byte offset.
 *  @param val  Byte to write.
 *  @return ALP_OK on success; ALP_ERR_INVAL on a NULL argument; otherwise the
 *          underlying I2C status. */
alp_status_t da9292_write_reg(da9292_t *ctx, uint8_t reg, uint8_t val);

/** @brief Release resources.  Idempotent.
 *
 *  Does not close the I2C bus handle -- the caller owns it.
 *
 *  @param ctx  Driver context (may already be deinitialised, or NULL). */
void da9292_deinit(da9292_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_DA9292_H */
