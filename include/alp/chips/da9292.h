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
 *   - **CH1** (phases 1 + 2) -> RZ/V2N 0.8 V rail.  Enabled at
 *     boot by the board-driven `EN1` strap; `critical` in the power
 *     tree, so software can never disable it.
 *   - **CH2** (phases 3 + 4) -> `VDD_0P75`, the DEEPX DX-M1 0.75 V core
 *     rail (V2N-M1 only).  Disabled at boot; brought up by
 *     da9292_ch2_sequence() -- run by U-Boot `board_late_init()` in
 *     A55-boot mode.  CM33-boot mode (ACT88760 GPIO5 `V2N_BOOT_CPU_SEL`)
 *     has NO owner yet: CA55/Linux is the sole RIIC8 / BRD_I2C master
 *     (`metadata/e1m_modules/v2n/core-ownership.yaml`), so the CM33
 *     cannot run it until that decision changes; see `boot_modes:` in
 *     `metadata/e1m_modules/v2n/power-tree.yaml`.  On V2N base CH2 stays
 *     disabled because DEEPX isn't populated.
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
 * Every read decodes through the LIVE `CHx_VSTEP` bit.  Guarded writes
 * (da9292_set_voltage_mv()) encode for the live VSTEP too, and the
 * guard compares the ENCODED voltage -- so a VSTEP=1 channel can never
 * be programmed to double its intended setpoint.  VSTEP itself is only
 * ever cleared, and only by da9292_ch2_sequence() while `CH2_EN = 0`
 * (datasheet: "The buck converter needs to be disabled (CHx_EN = 0)
 * before CHx_VSTEP setting can be changed by I2C write").
 *
 * @warning The DA9292-AROVx OTP variant used on V2N boots with
 *          `CH2_VSTEP=1` (PMC_CTRL_01 reset value 0x80, CH2 VSEL
 *          reading 1.80 V / 1.54 V).  Enabling CH2 in that state
 *          over-volts the DEEPX core -- use da9292_ch2_sequence().
 *
 * @par Guarded control (fail-closed)
 * With no limits table installed (da9292_set_limits()) every write --
 * voltage, enable/disable, raw register, sequence -- returns
 * ::ALP_ERR_NOSUPPORT.  With one installed, voltages must land inside
 * the channel window, a `critical` channel is never disabled, and
 * da9292_write_reg() only reaches PMC_EVENT_00/01 (0x02/0x03, W1C) and
 * PMC_MASK_00/01 (0x04/0x05); PMC_CTRL_01 and the VOUT registers are
 * typed-API-only, everything else is refused.
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
#include "alp/chips/pmic_rail_limit.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 7-bit I2C slave address used on V2N / V2N-M1.  The chip's
 *  `PMC_CFG_0A` register holds the address byte (`0x3C` 8-bit
 *  write-form for the AROVx OTP variant our SoMs use); the 7-bit
 *  bus address is `0x1E`. */
#define DA9292_I2C_ADDR_V2N 0x1Eu

/** Number of output channels in the V2N 2-channel dual-phase configuration. */
#define DA9292_CH_COUNT 2u

/** OTP identity triple (PMC_DEV_ID 0x19 / PMC_REV_ID 0x1A / PMC_CFG_REV 0x1B). */
typedef struct {
	uint8_t dev_id;  /**< PMC_DEV_ID (0xEA on the V2N-family variant). */
	uint8_t rev_id;  /**< PMC_REV_ID (0x42 on the bench unit). */
	uint8_t cfg_rev; /**< PMC_CFG_REV (0x11 on the V2N-family variant). */
} da9292_identity_t;

/** Decoded live state of one channel. */
typedef struct {
	bool     enabled;    /**< PMC_CTRL_01 CHx_EN. */
	bool     vstep;      /**< PMC_CTRL_01 CHx_VSTEP (1 = doubled 10 mV range). */
	bool     vsel_hi;    /**< PMC_CTRL_01 CHx_VSEL register bit (pin VSELx may also select). */
	uint16_t vsel_lo_mv; /**< VOUT_CHx_00 decoded through the live VSTEP (0 = reserved code). */
	uint16_t vsel_hi_mv; /**< VOUT_CHx_01 decoded through the live VSTEP (0 = reserved code). */
	bool     pg;         /**< PMC_STATUS_00 CHx_PG. */
	bool     uv;         /**< PMC_STATUS_00 CHx_UV. */
	bool     ov;         /**< PMC_STATUS_00 CHx_OV. */
	bool     oc;         /**< PMC_STATUS_00 CHx_OC. */
} da9292_channel_state_t;

/** Channel identifier (the two dual-phase output channels in V2N's
 *  CONF configuration). */
typedef enum {
	DA9292_CH1 = 0, /**< Phases 1 + 2 -- RZ/V2N 0.8 V rail. */
	DA9292_CH2 = 1, /**< Phases 3 + 4 -- VDD_0P75 DEEPX core rail on V2N-M1. */
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

/** Latched-event snapshot (`PMC_EVENT_00/01`). */
typedef struct {
	bool    e_ch1_pg;    /**< CH1 power-good edge. */
	bool    e_ch2_pg;    /**< CH2 power-good edge. */
	bool    e_ch1_uv;    /**< CH1 under-voltage event. */
	bool    e_ch2_uv;    /**< CH2 under-voltage event. */
	bool    e_ch1_ov;    /**< CH1 over-voltage event. */
	bool    e_ch2_ov;    /**< CH2 over-voltage event. */
	bool    e_ch1_oc;    /**< CH1 over-current event. */
	bool    e_ch2_oc;    /**< CH2 over-current event. */
	bool    e_temp_warn; /**< Thermal-warning event. */
	bool    e_temp_crit; /**< Thermal-critical event. */
	bool    e_vin_uvlo;  /**< Input UVLO event. */
	uint8_t raw_00;      /**< Untouched PMC_EVENT_00 byte. */
	uint8_t raw_01;      /**< Untouched PMC_EVENT_01 byte. */
} da9292_events_t;

/** Driver context.  Fields are driver-private; use the API. */
typedef struct {
	bool                     initialised; /**< Set by da9292_init(). */
	alp_i2c_t               *bus;         /**< BRD_I2C handle. */
	uint8_t                  addr;        /**< 7-bit slave address. */
	uint8_t                  dev_id;      /**< Cached PMC_DEV_ID (read at init). */
	uint8_t                  rev_id;      /**< Cached PMC_REV_ID. */
	const pmic_rail_limit_t *limits;      /**< DA9292_CH_COUNT entries, or NULL = fail-closed. */
} da9292_t;

/**
 * @brief Delay callback -- keeps the sequence OS-agnostic.
 *
 * Zephyr (CM33) passes a k_busy_wait()/k_msleep() wrapper; Linux (A55)
 * passes a usleep()/nanosleep() wrapper; U-Boot passes udelay().
 *
 * @param user  The @ref da9292_ch2_seq_cfg::delay_user pointer.
 * @param us    Microseconds to wait (at least).
 */
typedef void (*da9292_delay_fn)(void *user, uint32_t us);

/** Step at which da9292_ch2_sequence() stopped (DA9292_SEQ_OK = success). */
typedef enum {
	/** Rail up, PG held, DEEPX_CORE_0P75_EN high. */
	DA9292_SEQ_OK = 0,
	/** Bad cfg, no limits table, or target outside the CH2 window. */
	DA9292_SEQ_ERR_ARGS,
	/** PMC_DEV_ID != expected (or unreadable). */
	DA9292_SEQ_ERR_IDENTITY,
	/** PMC_STATUS_01 != 0x00 on entry (thermal / UVLO latched). */
	DA9292_SEQ_ERR_STATUS01,
	/** CTRL_01 / VOUT pre-program read failed. */
	DA9292_SEQ_ERR_PREREAD,
	/** VSTEP+EN clear write or its read-back failed. */
	DA9292_SEQ_ERR_CLEAR_VSTEP,
	/** VOUT_CH2_00/01 write or read-back failed. */
	DA9292_SEQ_ERR_PROGRAM_VOUT,
	/** CH1 VOUT changed or CH1_EN cleared. */
	DA9292_SEQ_ERR_CH1_DISTURBED,
	/** EVENT_00/01 read (pre-clear) failed. */
	DA9292_SEQ_ERR_EVENTS,
	/** DEEPX_PWR_EN_REQ never went high within the timeout. */
	DA9292_SEQ_ERR_NO_REQUEST,
	/** CTRL_01 re-read right before CH2_EN shows VSTEP=1. */
	DA9292_SEQ_ERR_VSTEP_AT_ENABLE,
	/** CH2_EN write failed. */
	DA9292_SEQ_ERR_ENABLE,
	/** No CH2_PG (or UV/OV/OC, or a latched OV/OC) within the PG timeout. */
	DA9292_SEQ_ERR_PG_TIMEOUT,
	/** PG lost / UV after DEEPX_CORE_0P75_EN went high. */
	DA9292_SEQ_ERR_PG_DROPPED,
} da9292_ch2_seq_step_t;

/** Inputs for da9292_ch2_sequence(). */
struct da9292_ch2_seq_cfg {
	/** CH2 setpoint (750 on V2N-M1); must lie inside the installed CH2 window. */
	uint16_t target_mv;
	/** Required PMC_DEV_ID (0xEA; generated `V2N_POWER_DA9292_DEV_ID`). */
	uint8_t expected_dev_id;
	/** Opened input on `DEEPX_PWR_EN_REQ` (V2N P65).  Required. */
	alp_gpio_t *pwr_en_req;
	/** Opened output on `DEEPX_CORE_0P75_EN` (V2N P64).  Required; driven
	 *  low on every abort. */
	alp_gpio_t *core_en;
	/** Opened output on `M1_RESET` (V2N PA6), or NULL.  Driven low (reset
	 *  asserted) on every abort; never released by the sequence. */
	alp_gpio_t *m1_reset;
	/** DEEPX_PWR_EN_REQ poll budget in ms (U-Boot: 500). */
	uint32_t req_timeout_ms;
	/** CH2_PG poll budget after CH2_EN in ms (U-Boot: 20). */
	uint32_t pg_timeout_ms;
	/** Settle after DEEPX_CORE_0P75_EN high, before the PG re-check, in ms (U-Boot: 5). */
	uint32_t pg_settle_ms;
	/** Delay callback.  Required. */
	da9292_delay_fn delay;
	/** Opaque pointer passed to `delay`. */
	void *delay_user;
};

/** Diagnostics filled by da9292_ch2_sequence() on every return path. */
struct da9292_ch2_seq_result {
	/** Where it stopped (DA9292_SEQ_OK on success). */
	da9292_ch2_seq_step_t step;
	/** Warm-reboot path: VSTEP=0 and both CH2 VOUT already == target, so no
	 *  program-phase writes were issued. */
	bool already_programmed;
	/** CH2 PG confirmed and DEEPX_CORE_0P75_EN left high. */
	bool rail_up;
	/** Identity as read. */
	da9292_identity_t identity;
	/** Last PMC_CTRL_01 read. */
	uint8_t ctrl_01;
	/** Last VOUT_CH2_00 read. */
	uint8_t vout_ch2_00;
	/** Last VOUT_CH2_01 read. */
	uint8_t vout_ch2_01;
	/** Last PMC_STATUS_00 read. */
	uint8_t status_00;
	/** Last PMC_STATUS_01 read. */
	uint8_t status_01;
	/** Last PMC_EVENT_00 read. */
	uint8_t event_00;
	/** Last PMC_EVENT_01 read. */
	uint8_t event_01;
	/** PMC_CFG_00 (0x0E) as read in step 4.  The AROVx OTP ships 0xFF:
	 *  the EN2_EN / VSEL2_EN pin functions are ON, so a GPIO driving the
	 *  EN2 pin turns CH2 on at the OTP default (VSTEP=1, 1.80 V) with no
	 *  I2C guard in the path. */
	uint8_t pmc_cfg_00;
};

/**
 * @brief Probe the chip and cache its device + revision identifiers.
 *
 * Does not change any register -- the chip's OTP carries the V2N
 * power-on defaults and CH1 enables itself from the EN1 strap.
 * Clears any installed limits table.
 *
 * @param ctx        Driver context (output).
 * @param bus        BRD_I2C handle.
 * @param addr_7bit  7-bit slave address (DA9292_I2C_ADDR_V2N on V2N).
 * @return ALP_OK; ALP_ERR_INVAL on NULL args or an address > 0x7F;
 *         ALP_ERR_NOT_READY if the chip does not ACK.
 */
alp_status_t da9292_init(da9292_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit);

/**
 * @brief Install the guard table that unlocks every control write.
 *
 * Stores the pointer only; @p ch_limits must outlive @p ctx (static
 * storage initialised from the generated `V2N_POWER_DA9292_CH_LIMITS_INIT`
 * / `V2N_M1_POWER_DA9292_CH_LIMITS_INIT`).  NULL uninstalls (fail-closed).
 *
 * @param ctx        DA9292 context handle (initialised).
 * @param ch_limits  DA9292_CH_COUNT entries indexed by ::da9292_channel_t, or NULL.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL if a
 *         voltage-writable entry has min_mv > max_mv.
 */
alp_status_t da9292_set_limits(da9292_t *ctx, const pmic_rail_limit_t *ch_limits);

/**
 * @brief Read PMC_DEV_ID / PMC_REV_ID / PMC_CFG_REV.
 *
 * @param ctx  DA9292 context handle (must be initialised first).
 * @param out  Receives the identity triple.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL on
 *         NULL @p out; the bus status on I2C failure.
 */
alp_status_t da9292_get_identity(da9292_t *ctx, da9292_identity_t *out);

/**
 * @brief Read one channel's enable / VSTEP / setpoints / status bits.
 *
 * @param ctx  DA9292 context handle (must be initialised first).
 * @param ch   Channel.
 * @param out  Receives the decoded state.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL on
 *         NULL @p out or an invalid channel; the bus status on I2C failure.
 */
alp_status_t
da9292_get_channel_state(da9292_t *ctx, da9292_channel_t ch, da9292_channel_state_t *out);

/**
 * @brief Read both status bytes and decode the bit fields.
 *
 * @param ctx  DA9292 context handle (must be initialised first).
 * @param out  Receives the decoded live status.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL on
 *         NULL @p out; the bus status on I2C failure.
 */
alp_status_t da9292_get_status(da9292_t *ctx, da9292_status_t *out);

/**
 * @brief Read `PMC_EVENT_00/01` WITHOUT clearing them.
 *
 * @param ctx  DA9292 context handle (must be initialised first).
 * @param out  Receives the latched events.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL on
 *         NULL @p out; the bus status on I2C failure.
 */
alp_status_t da9292_peek_events(da9292_t *ctx, da9292_events_t *out);

/**
 * @brief Read `PMC_EVENT_00/01`, then write-1-to-clear exactly the bits read.
 *
 * Clearing an event register is a control write: with no limits table
 * installed the events are returned but NOT cleared and the call returns
 * ::ALP_ERR_NOSUPPORT.
 *
 * @param ctx  DA9292 context handle (must be initialised first).
 * @param out  Receives the latched events as read before the clear.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL on
 *         NULL @p out; ALP_ERR_NOSUPPORT if no table is installed (@p out
 *         still filled); the bus status on I2C failure.
 */
alp_status_t da9292_read_and_clear_events(da9292_t *ctx, da9292_events_t *out);

/**
 * @brief Sample the DA9292 fault pins and pack them into a flag byte.
 *
 * Reads two already-opened GPIO inputs wired to the chip's open-drain,
 * active-low fault outputs -- on V2N: `INT_N` = Renesas `P37`
 * (`DA9292_INT`), `TW_N` = Renesas `P36` (`DA9292_TW`) -- and packs
 * bit0 = `INT_N` asserted (pin low), bit1 = `TW_N` asserted (pin low),
 * bits 2-7 = 0.  The packing matches the GD32 bridge's
 * `DA9292_STATUS_FORWARD` (opcode `0x40`) reply byte; on the current SoM
 * revision the bridge answers `0xFF` ("no sample") and this direct read is
 * the working fault-pin path.
 *
 * @param int_n  Opened input on the `DA9292_INT` net, or NULL to skip
 *               (bit0 then reports deasserted).
 * @param tw_n   Opened input on the `DA9292_TW` net, or NULL to skip
 *               (bit1 then reports deasserted).
 * @param flags  Receives the packed byte.  Required.
 * @return ALP_OK; ALP_ERR_INVAL on NULL @p flags; or the first failing
 *         `alp_gpio_read` status.
 */
alp_status_t da9292_get_fault_pins(alp_gpio_t *int_n, alp_gpio_t *tw_n, uint8_t *flags);

/**
 * @brief Set or clear a channel's register-side enable (`CHx_EN`) -- guarded.
 *
 * Enabling additionally refuses (::ALP_ERR_OUT_OF_RANGE) when the live
 * setpoint, decoded through the live VSTEP, lies outside the channel
 * window -- an enable can never switch a channel on at an out-of-window
 * voltage (the VSTEP=1 OTP default on CH2 is exactly that case).  The
 * channel is the AND of this bit and the ENx pin where the pin is routed.
 *
 * @param ctx     DA9292 context handle (must be initialised first).
 * @param ch      Channel.
 * @param enable  true = set CHx_EN, false = clear it.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL on an
 *         invalid channel; ALP_ERR_NOSUPPORT if no table is installed, the
 *         channel is not enable-writable, or @p enable is false on a
 *         `critical` channel; ALP_ERR_OUT_OF_RANGE as above; the bus
 *         status on I2C failure.
 */
alp_status_t da9292_set_enable(da9292_t *ctx, da9292_channel_t ch, bool enable);

/**
 * @brief Program a channel's setpoint in millivolts -- window-guarded.
 *
 * Encodes @p mv for the channel's LIVE VSTEP (5 mV grid at VSTEP=0,
 * 10 mV at VSTEP=1; rounded down), refuses unless the ENCODED voltage is
 * inside the channel window, then writes BOTH `CHx_VOUT_VSEL_LO` and
 * `CHx_VOUT_VSEL_HI` (VSELx pin routing is undocumented on V2N) and reads
 * both back.  VSTEP is never changed here.
 *
 * @warning Don't ramp by big steps -- the chip flags instantaneous OV/UV
 *          when the target jumps by more than ~50 mV in one write.
 *
 * @param ctx  DA9292 context handle (must be initialised first).
 * @param ch   Channel.
 * @param mv   Requested setpoint.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL on an
 *         invalid channel; ALP_ERR_NOSUPPORT if no table is installed or
 *         the channel is not voltage-writable; ALP_ERR_OUT_OF_RANGE if the
 *         encoded voltage is outside the window or the code space;
 *         ALP_ERR_IO on a read-back mismatch; the bus status on I2C failure.
 */
alp_status_t da9292_set_voltage_mv(da9292_t *ctx, da9292_channel_t ch, uint16_t mv);

/**
 * @brief Read the channel's ACTIVE setpoint in mV (VSEL_LO or VSEL_HI per
 *        the CHx_VSEL register bit, decoded through the live VSTEP).
 *
 * @param ctx  DA9292 context handle (must be initialised first).
 * @param ch   Channel.
 * @param mv   Receives the setpoint.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL on
 *         NULL @p mv or an invalid channel; ALP_ERR_IO on a reserved code;
 *         the bus status on I2C failure.
 */
alp_status_t da9292_get_voltage_mv(da9292_t *ctx, da9292_channel_t ch, uint16_t *mv);

/**
 * @brief Bring up CH2 (`VDD_0P75`, DEEPX core) -- the proven U-Boot sequence
 *        as an OS-agnostic driver function.
 *
 * Builds for Zephyr and Linux / U-Boot; every OS dependency is a
 * caller-opened ::alp_gpio_t or the delay callback.  Only the one owner
 * recorded for the running boot mode may call it (A55-boot: U-Boot;
 * CM33-boot: blocked -- the CM33 must not master RIIC8 today).
 *
 * @warning The DA9292-AROVx OTP enables the EN2 / VSEL2 pin functions
 *          (PMC_CFG_00 = 0xFF, read into @p res): driving the EN2 pin
 *          before this sequence finishes brings CH2 up at the OTP
 *          default (VSTEP=1, 1.80 V) on the 0.75 V DEEPX core, with no
 *          I2C guard in the path.  The EN2 net is still TBD.  Step by step (mirrors
 * `meta-alp-sdk/recipes-bsp/u-boot/u-boot/0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch`):
 *  1. refuse unless a limits table is installed and @p cfg->target_mv lies
 *     inside the CH2 window;
 *  2. PMC_DEV_ID must equal @p cfg->expected_dev_id (REV_ID / CFG_REV are
 *     read into the result either way);
 *  3. PMC_STATUS_01 must read 0x00;
 *  4. read CTRL_01, VOUT_CH2_00/01, VOUT_CH1_00 and PMC_CFG_00 (EN2 /
 *     VSEL2 pin-function enables, recorded in the result);
 *  5. warm-reboot idempotent path: VSTEP=0 and both CH2 VOUT == target
 *     -> skip 6-7 entirely (a live CH2_EN is NEVER cleared);
 *  6. else clear CH2_EN first, then CH2_VSTEP (VSTEP may only change with
 *     the channel off), each by read-modify-write and read back; refuse
 *     (::ALP_ERR_NOSUPPORT) to switch off a live CH2 marked `critical`;
 *  7. write VOUT_CH2_00 and VOUT_CH2_01 = target at VSTEP=0 (0x96 for
 *     750 mV) and read both back;
 *  8. (every path, warm or cold) CH1: VOUT_CH1_00 unchanged and CH1_EN
 *     still set;
 *  9. read EVENT_00/01 and write-1-to-clear what was read (a failed
 *     clear aborts);
 * 10. poll @p cfg->pwr_en_req high for up to @p cfg->req_timeout_ms; on a
 *     warm reboot with CH2 already live a timeout returns
 *     ::ALP_ERR_NOT_READY WITHOUT touching CH2_EN or the pins;
 * 11. re-read CTRL_01; abort if CH2_VSTEP is set (clearing a set CH2_EN);
 *     else set CH2_EN (read-modify-write; no write when already set);
 * 12. poll STATUS_00 up to @p cfg->pg_timeout_ms for CH2_PG with
 *     UV/OV/OC clear, then require EVENT_00 free of CH2_OV/CH2_OC; on
 *     failure clear CH2_EN;
 * 13. drive @p cfg->core_en high, wait @p cfg->pg_settle_ms, re-check PG
 *     with no UV / OV / OC in STATUS_00 and no CH2 OV / OC in EVENT_00
 *     (the OTP masks OV out of PG); on failure drive core_en low FIRST,
 *     then clear CH2_EN.
 * Every abort from step 2 on (except the warm-live step-10 timeout)
 * drives @p cfg->core_en low and (when given)
 * @p cfg->m1_reset low; this function never releases M1_RESET -- that is
 * the caller's PCIe bring-up step, only after ALP_OK.
 *
 * @param ctx  DA9292 context handle (must be initialised first).
 * @param cfg  Sequence inputs (GPIOs already opened with the right direction).
 * @param res  Diagnostics, filled on every return path (may be NULL).
 * @return ALP_OK when CH2 is up with PG held and core_en high;
 *         ALP_ERR_NOT_READY if not initialised or DEEPX_PWR_EN_REQ never
 *         rose; ALP_ERR_INVAL on NULL / missing required cfg members;
 *         ALP_ERR_NOSUPPORT if no table is installed or the identity does
 *         not match; ALP_ERR_OUT_OF_RANGE if the target is outside the CH2
 *         window; ALP_ERR_BUSY if STATUS_01 is non-zero; ALP_ERR_TIMEOUT on
 *         a PG timeout; ALP_ERR_IO on a read-back mismatch, a disturbed CH1,
 *         VSTEP at enable time, a dropped PG, or a bus failure.
 */
alp_status_t da9292_ch2_sequence(da9292_t                        *ctx,
                                 const struct da9292_ch2_seq_cfg *cfg,
                                 struct da9292_ch2_seq_result    *res);

/**
 * @brief Raw register read (any address).
 *
 * @param ctx  DA9292 context handle (must be initialised first).
 * @param reg  Register address.
 * @param val  Receives the byte.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_INVAL on
 *         NULL @p val; the bus status on I2C failure.
 */
alp_status_t da9292_read_reg(da9292_t *ctx, uint8_t reg, uint8_t *val);

/**
 * @brief Raw register write -- only PMC_EVENT_00/01 (0x02/0x03) and
 *        PMC_MASK_00/01 (0x04/0x05) are reachable.
 *
 * @param ctx  DA9292 context handle (must be initialised first).
 * @param reg  Register address.
 * @param val  Byte to write.
 * @return ALP_OK; ALP_ERR_NOT_READY if not initialised; ALP_ERR_NOSUPPORT
 *         if no table is installed or @p reg is not raw-writable; the bus
 *         status on I2C failure.
 */
alp_status_t da9292_write_reg(da9292_t *ctx, uint8_t reg, uint8_t val);

/**
 * @brief Release resources.  Idempotent.
 *
 * @param ctx  DA9292 context handle (may be NULL).
 */
void da9292_deinit(da9292_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_DA9292_H */
