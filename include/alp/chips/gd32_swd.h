/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file gd32_swd.h
 * @brief Bit-bang SWD controller for flashing the GD32G553 over GPIO.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * The companion GD32G553MEY7TR on the E1M-X V2N / V2N-M1 SoMs is
 * a Cortex-M33 with the standard Arm Coresight SWD debug port.
 * Per the 2026-05-12 hardware decision the V2N board routes
 * `GD32_SWDIO` + `GD32_SWCLK` + `GD32_NRST` from the Renesas RZ/V2N
 * host back to the GD32 so the host can reflash the supervisor MCU
 * in the field without an external probe.
 *
 * @par Driver status: PARTIAL
 *
 * Packet layer + DPIDR read + Cortex-M33 halt + FMC erase/write/verify
 * + reset-and-run coded and ready for first-silicon exercise.  Real
 * verification tracked in `docs/test-plan.md`.
 *
 * @par Pin model on V2N
 *
 * | Signal       | Renesas pad | GD32 pad | Notes                                      |
 * |--------------|-------------|----------|--------------------------------------------|
 * | `GD32_SWDIO` | `P70`       | `PA13`   | bidirectional; was GPT0_GTIOC0A / PWM2     |
 * | `GD32_SWCLK` | `P71`       | `PA14`   | host drives; was GPT0_GTIOC0B / PWM3       |
 * | `GD32_NRST`  | `P74`       | `NRST`   | open-drain; shared with PMIC reset out     |
 *
 * The caller opens three `alp_gpio_t` handles, hands them to
 * `gd32_swd_init`, and retains ownership: `gd32_swd_deinit` does not
 * close them.  `SWDIO` switches between input and output transparently
 * inside the driver during data phases.
 *
 * @par Reference
 *
 * Arm DDI 0316C "ARM Debug Interface v5" + Arm DUI 0552A "Coresight
 * DAP Bit-bang Algorithms" specify the wire protocol; both are
 * publicly available from Arm's developer docs.
 */

#ifndef ALP_CHIPS_GD32_SWD_H
#define ALP_CHIPS_GD32_SWD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Generic ADIv5 SW-DP IDCODE for a Cortex-M33 r0p1 SW-DPv2.
 *
 *  @warning UNVERIFIED on a GD32G553 -- this value comes from the core's
 *  generic architectural expectation, not from a probe on the part. It is
 *  also, separately, the bench-measured SW-DP ID of the V2N CM33 DAP on
 *  a V2N bench unit (`Found SW-DP with ID 0x6BA02477`, `Found Cortex-M33
 *  r0p4`) -- a *different* target on the same board
 *  (`scripts/bench/aen/bench-env.sh`). Whether a real GD32G553 answers
 *  this value, the only other GD32 candidate on record (`0x0BE12477`,
 *  itself with no attribution at all -- no bench transcript, no
 *  datasheet reference, no commit message), or something else is
 *  UNKNOWN: neither has been measured on a GD32 with a probe attached
 *  (alp-sdk#1440, #1369). A comparison against this macro therefore
 *  proves nothing about the target either way -- do NOT make a mismatch
 *  fatal (`gd32_swd_connect()` deliberately does not), and do NOT treat
 *  a mismatch as confirmation of wrong silicon or mis-wiring. Settling
 *  it needs a probe on a GD32. A production test that wants to refuse
 *  on a mismatch should match against a value measured on its own board.
 *
 *  `metadata/chips/gd32_swd.yaml` does NOT carry this value --
 *  `target_expected_idcode` is deliberately absent there, same stance
 *  `metadata/schemas/soc-spec-v1.schema.json`'s own `expect_dpidr` field
 *  guidance takes for every Alif Ensemble SoC variant (#1355): an absent
 *  key is the correct published "unknown", and a guessed value is
 *  strictly worse than absent. This macro stays only as the
 *  informational generic-architecture reference that the driver comment
 *  and the v2n-gd32-swd-flash example log; neither gates on it. */
#define GD32_SWD_GENERIC_CM33_R0P1_IDCODE 0x6BA02477u

/** Default clock-delay loop count.  Higher = slower SWCLK. */
#define GD32_SWD_DEFAULT_CLOCK_DELAY 4u

/** Maximum number of retry rounds for `ACK_WAIT` from the target. */
#define GD32_SWD_MAX_WAIT_RETRIES 16u

/** GD32G553 flash base address (per the GD32G553 datasheet). */
#define GD32_SWD_FMC_FLASH_BASE 0x08000000u

/** Sector size on the GD32G553 in bytes. */
#define GD32_SWD_FMC_SECTOR_BYTES 2048u

/** SRAM base, used internally to stage operations. */
#define GD32_SWD_FMC_SRAM_BASE 0x20000000u

/** ACK response (3-bit field returned by the target after every
 *  request).  Values match the Arm ADIv5 specification. */
typedef enum {
	GD32_SWD_ACK_OK    = 0x1,
	GD32_SWD_ACK_WAIT  = 0x2,
	GD32_SWD_ACK_FAULT = 0x4,
	GD32_SWD_ACK_PROTO = 0x7,
} gd32_swd_ack_t;

/** Driver context. */
typedef struct {
	bool        initialised;
	bool        connected;
	alp_gpio_t *swdio;
	alp_gpio_t *swclk;
	alp_gpio_t *nrst;
	uint32_t    clock_delay;
	uint32_t    idcode;
	bool        swdio_is_output;
	bool        nrst_held; /**< GD32_NRST asserted (connect-under-reset). */
} gd32_swd_t;

/**
 * @brief Bind the controller to caller-supplied GPIO handles.
 *
 * CONNECT-UNDER-RESET.  `swclk` (Renesas `P71`) doubles as the bridge's
 * ATTN input (GD32 `PA14`), which the GD32 drives while the v0.15 ATTN
 * link feature is granted, so the host may only drive it as an output while
 * `GD32_NRST` (`P74`) holds the GD32 in reset (docs/gd32-bridge-protocol.md
 * §3.17 rule H3).  This call therefore:
 *   1. notifies the platform (@ref gd32_swd_session_notify) so the SPI
 *      bridge link is closed and stays closed until @ref gd32_swd_deinit;
 *   2. asserts `nrst` (output, driven low -- emulated open-drain: it is
 *      never driven high) and FAILS if that does not work;
 *   3. only then switches `swdio` / `swclk` to outputs at the SWD idle state.
 * `nrst` stays asserted through @ref gd32_swd_connect, which arms
 * DEMCR.VC_CORERESET and releases it so the core halts at its reset vector
 * with no application code run.  `nrst` is MANDATORY: recovery without it is
 * not supported, and a NULL `nrst` is @ref ALP_ERR_INVAL.  Ownership of the
 * handles stays with the caller.
 *
 * On the V2N CM33 boards the three handles come from
 * @ref GD32G553_PAD_ID_SWDIO / @ref GD32G553_PAD_ID_SWCLK /
 * @ref GD32G553_PAD_ID_NRST via `alp_gpio_open()` (resolved from the
 * board's `alp,gd32-pads` devicetree node, never from the positional
 * `alp,pin-array`).
 *
 * @return @ref ALP_OK on success, @ref ALP_ERR_INVAL on a NULL ctx / swdio /
 *         swclk / nrst, or the GPIO error from asserting NRST or from the
 *         idle-state configuration (in which case the pads are back to
 *         inputs, NRST is released and the session is closed).
 */
alp_status_t gd32_swd_init(gd32_swd_t *ctx, alp_gpio_t *swdio, alp_gpio_t *swclk, alp_gpio_t *nrst);

/** Override the per-half-bit clock-delay loop count.  Clamped to
 *  [0, 2048]. */
alp_status_t gd32_swd_set_clock_delay(gd32_swd_t *ctx, uint32_t delay_spins);

/**
 * @brief Perform line-reset + JTAG-to-SWD switch + DPIDR read.
 *
 * On success the IDCODE is cached in @ref gd32_swd_t::idcode.
 */
alp_status_t gd32_swd_connect(gd32_swd_t *ctx);

/** Halt the Cortex-M33 cleanly via the Debug Halting Control and
 *  Status Register.  Must be called before any flash op. */
alp_status_t gd32_swd_halt(gd32_swd_t *ctx);

/**
 * @brief Erase the smallest enclosing range of flash sectors that
 *        covers @p addr .. @p addr + @p size - 1.
 *
 * Address + size are rounded out to sector boundaries
 * (@ref GD32_SWD_FMC_SECTOR_BYTES).
 */
alp_status_t gd32_swd_flash_erase(gd32_swd_t *ctx, uint32_t addr, uint32_t size);

/** Program @p len bytes from @p data into flash starting at @p addr.
 *  Destination must be erased.  Doubleword-aligned. */
alp_status_t gd32_swd_flash_write(gd32_swd_t *ctx, uint32_t addr, const uint8_t *data, size_t len);

/** Read @p len bytes from flash starting at @p addr and compare
 *  against @p data.  Returns @ref ALP_OK on full match. */
alp_status_t gd32_swd_flash_verify(gd32_swd_t *ctx, uint32_t addr, const uint8_t *data, size_t len);

/** Release the core from debug halt + issue a system reset.  Uses
 *  the hardware @c NRST line when wired, otherwise the standard
 *  AIRCR.SYSRESETREQ + VECTKEY write. */
alp_status_t gd32_swd_reset_and_run(gd32_swd_t *ctx);

/** Release the driver context: releases NRST, returns `swdio` and `swclk`
 *  (P70 / P71) to inputs and closes the session (@ref gd32_swd_session_notify
 *  with false).  Does NOT close the GPIO handles. */
void gd32_swd_deinit(gd32_swd_t *ctx);

/**
 * @brief Session hook: an SWD session starts (true) or ends (false).
 *
 * Weak no-op in the driver; the platform glue that owns the GD32 bridge link
 * overrides it (src/zephyr/v2n_supervisor.c).  Called with true before
 * @ref gd32_swd_init touches any pad and with false when the pads are
 * inputs again, so the bridge can be closed, kept from re-initialising or
 * renegotiating, and answer BUSY for the whole session -- the bridge and
 * this driver share P71.
 */
void gd32_swd_session_notify(bool active);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_GD32_SWD_H */
