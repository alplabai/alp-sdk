/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tcal9538.h
 * @brief TI TCA9538 / TCAL9538 8-channel I2C I/O expander driver.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Both parts share the same base register layout (0x00..0x03).  The
 * TCAL9538 additionally exposes an "Agile IO" register block (0x40+)
 * with per-pin latched-interrupt, interrupt-mask/status, and
 * pull-up/pull-down control -- see `tcal9538_set_input_latch()`,
 * `tcal9538_set_interrupt_mask()`, `tcal9538_get_interrupt_status()`,
 * and `tcal9538_set_pull()` below.  The register-compatible TCA6408A
 * alt-population has none of these registers; the four Agile-IO calls
 * return ALP_ERR_NOSUPPORT on that part -- see
 * `tcal9538_t::has_latched_irq`.  Eight I/O pins, individually
 * configurable as inputs or outputs, with optional polarity
 * inversion.  7-bit address strap A1A0 selects 0x70..0x73
 * (TCA9538/TCAL9538).
 *
 * On the E1M EVK, R1 ONLY: the expander's own \\INT output (open-drain,
 * active low) fed `IO_EXP.INT` -> edge pin Z2, pad AUDIO_CLK, with a
 * 10k pull-up (R140) -- that pad was unavailable while the audio path
 * was active, so a consumer had to fall back to polling
 * `tcal9538_get_interrupt_status()` (or the raw input port) whenever
 * audio was in use.  On R2 the maintainer has confirmed AUDIO_CLK/Z2
 * is free -- this repurposing no longer applies, and where \\INT lands
 * on R2 (if anywhere) is UNVERIFIED pending an R2 netlist read; do not
 * assume unrouted.  See the `EVK_PIN_IO_EXP_INT` entry in
 * `metadata/boards/e1m-evk.yaml`'s `overlay_pins:` block.
 *
 * This driver also drives the register-compatible TCA6408A/PCA9538
 * family (single A0 strap -> 0x20..0x21) used as the E1M EVK's U35
 * alt-population (TCA6408ARSVR, R112 fitted / R145 DNP).  0x20 was
 * BENCH-CONFIRMED 2026-06-16 on an EARLIER E1M EVK revision, but is
 * NOT ASSEMBLED on the current (Alif) revision -- neither of two
 * 2026-09-05 boards answers there (alp-sdk#1974) -- see
 * `EVK_I2C_ADDR_TCA6408A_MAIN_NOT_ASSEMBLED` in
 * `<alp/boards/alp_e1m_evk_routes.h>`.  `tcal9538_init()` therefore
 * accepts both strap families; do not re-narrow the address check to
 * the TCAL9538-only range (0x70..0x73) without first checking for
 * TCA6408A-populated boards.
 *
 * On the current E1M EVK revision the chip sits on ALP_E1M_I2C0 at
 * 0x73 (A1=1, A0=1) -- CORRECTED 2026-09-05 from 0x72 (alp-sdk#1974) --
 * and fans out LCD / camera / capacitive-touch control lines plus
 * four sensor interrupt inputs.  See
 * `<alp/boards/alp_e1m_evk.h>`'s `evk_ioexp_pin_t` enum
 * for the EVK-side pin layout.
 *
 * Register map per TI TCAL9538 datasheet (SCPS280B), Table 7-3
 * (p.24) -- base registers are shared with the TCA6408A/PCA9538 alt
 * part, the 0x40+ block is TCAL9538-only:
 *   0x00  Input port          (RO)  Reflects external pin levels.
 *                                    Reading this register clears any
 *                                    latched/pending interrupt
 *                                    (SCPS280B p.25).
 *   0x01  Output port         (RW)  Drives output pins (where
 *                                    Configuration bit = 0).
 *   0x02  Polarity            (RW)  XOR mask for the Input register.
 *   0x03  Configuration       (RW)  1 = input, 0 = output.  Default:
 *                                    all inputs (0xFF).
 *   0x42  Input latch         (RW)  TCAL9538-only.  Bit = 1 latches
 *                                    that input pin's transition until
 *                                    register 0x00 is read; bit = 0
 *                                    (default) leaves the pin
 *                                    unlatched (SCPS280B p.25-26,
 *                                    Table 7-9).
 *   0x43  Pull enable         (RW)  TCAL9538-only.  Bit = 1 enables
 *                                    that pin's internal 100k
 *                                    pull-up/pull-down; default 0x00
 *                                    (disabled) (SCPS280B p.26,
 *                                    Table 7-10).
 *   0x44  Pull select         (RW)  TCAL9538-only.  Bit = 1 selects
 *                                    pull-up, 0 selects pull-down for
 *                                    a pin with its 0x43 enable bit
 *                                    set; default 0xFF (SCPS280B p.26,
 *                                    Table 7-11).
 *   0x45  Interrupt mask      (RW)  TCAL9538-only.  Bit = 1 masks
 *                                    (disables) that pin's interrupt;
 *                                    default 0xFF, i.e. all pins
 *                                    masked at power-up (SCPS280B
 *                                    p.26, Table 7-12).
 *   0x46  Interrupt status    (RO)  TCAL9538-only.  Bit = 1 means that
 *                                    pin is the source of the current
 *                                    \\INT assertion (masked pins
 *                                    always read 0).  This register is
 *                                    NOT clear-on-read -- only reading
 *                                    register 0x00 clears the
 *                                    condition (SCPS280B p.26,
 *                                    Table 7-13).
 */

#ifndef ALP_CHIPS_TCAL9538_H
#define ALP_CHIPS_TCAL9538_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCAL9538_I2C_ADDR_BASE 0x70u /**< A1=0, A0=0. Strap range: BASE..+3 (0x70..0x73). */

/**
 * TCA6408A/PCA9538 alt-part single-strap base (A0=0).  Strap range:
 * ALT_BASE..+1 (0x20..0x21).  0x20 was BENCH-CONFIRMED 2026-06-16 on an
 * EARLIER E1M EVK revision; NOT ASSEMBLED on the current one (alp-sdk#1974)
 * -- see EVK_I2C_ADDR_TCA6408A_MAIN_NOT_ASSEMBLED in
 * <alp/boards/alp_e1m_evk_routes.h>.
 *
 * Kept on ONE line deliberately: scripts/abi_snapshot.py only captures a
 * macro's value when the `#define` is not line-continued, so a wrapped form
 * records an empty value and the ABI gate can no longer see this address
 * change.  Keep this doc block ABOVE the define -- a trailing doc comment
 * pushes the line past the column limit and clang-format wraps it again.
 */
#define TCAL9538_I2C_ADDR_ALT_BASE 0x20u

typedef enum {
	TCAL9538_DIR_OUTPUT = 0, /**< Configuration bit = 0. */
	TCAL9538_DIR_INPUT  = 1, /**< Configuration bit = 1. */
} tcal9538_direction_t;

/** Internal pull resistor selection for `tcal9538_set_pull()`.  Maps onto
 *  the 0x43 (enable) / 0x44 (select) register pair -- TCAL9538-only. */
typedef enum {
	TCAL9538_PULL_NONE = 0, /**< 0x43 bit = 0: pull resistor disabled. */
	TCAL9538_PULL_DOWN = 1, /**< 0x43 bit = 1, 0x44 bit = 0: 100k pull-down. */
	TCAL9538_PULL_UP   = 2, /**< 0x43 bit = 1, 0x44 bit = 1: 100k pull-up. */
} tcal9538_pull_t;

typedef struct {
	bool       initialised;
	alp_i2c_t *bus;
	uint8_t    addr;
	/* Cached register state -- keeps the driver from a
     * read-modify-write cycle on every set_pin / set_direction call.
     * Synced with the chip on init via a register read-back. */
	uint8_t cfg_cache; /**< Configuration reg (0x03). */
	uint8_t out_cache; /**< Output port reg (0x01). */
	/** Capability flag, set at init from the matched I2C address strap
	 *  range: true when @p addr fell in the TCAL9538 range
	 *  (0x70..0x73), false for the register-compatible TCA6408A/PCA9538
	 *  alt-population (0x20..0x21), which has no 0x40+ register block.
	 *  Every Agile-IO call (`tcal9538_set_input_latch()`,
	 *  `tcal9538_set_interrupt_mask()`, `tcal9538_get_interrupt_status()`,
	 *  `tcal9538_set_pull()`) checks this flag and returns
	 *  ALP_ERR_NOSUPPORT instead of touching the bus when it is false --
	 *  the part has no ID register to probe, so the init-time strap
	 *  range is the only signal available. */
	bool has_latched_irq;
} tcal9538_t;

/**
 * @brief Probe + cache the chip's register state.
 *
 * @param[out] ctx       Driver context (output; populated on success).
 * @param[in]  bus       Open I2C bus handle the expander sits on.
 * @param[in]  addr_7bit 7-bit I2C address, either a TCA9538/TCAL9538
 *                       strap (0x70..0x73) or a register-compatible
 *                       TCA6408A/PCA9538 strap (0x20..0x21 -- see
 *                       TCAL9538_I2C_ADDR_ALT_BASE).  Use 0 to fall
 *                       back to TCAL9538_I2C_ADDR_BASE.
 */
alp_status_t tcal9538_init(tcal9538_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit);

/**
 * @brief Set the direction (input/output) of a single pin.
 *
 * @param ctx  TCAL9538 driver context (must be initialised first).
 * @param pin  0..7
 * @param dir  One of @ref tcal9538_direction_t.
 */
alp_status_t tcal9538_set_direction(tcal9538_t *ctx, uint8_t pin, tcal9538_direction_t dir);

/**
 * @brief Set directions of multiple pins via mask + value.
 * Bit N of @p mask = 1 means "apply"; bit N of @p value = 1 means
 * "input", 0 means "output".  This is the bulk variant of
 * `tcal9538_set_direction` and avoids 8 round-trips for typical
 * "configure 4 outputs at once" calls.
 */
alp_status_t tcal9538_set_directions(tcal9538_t *ctx, uint8_t mask, uint8_t value);

/** @brief Drive a single output pin to @p level. */
alp_status_t tcal9538_set(tcal9538_t *ctx, uint8_t pin, bool level);

/** @brief Read a single input pin's current level. */
alp_status_t tcal9538_get(tcal9538_t *ctx, uint8_t pin, bool *level_out);

/** @brief Read all 8 input pins as a bitmap. */
alp_status_t tcal9538_read_all(tcal9538_t *ctx, uint8_t *port_out);

/** @brief Write all 8 output pins at once.  Bits with direction =
 *  input are ignored by the chip. */
alp_status_t tcal9538_write_all(tcal9538_t *ctx, uint8_t port);

/**
 * @brief Set the polarity-inversion mask (register 0x02).
 *
 * Base register -- shared with the TCA6408A/PCA9538 alt-population,
 * unlike the 0x40+ Agile-IO block. Bit N of @p mask = 1 XORs pin N's
 * electrical level before it lands in the input port register (0x00);
 * bit N = 0 (the power-up default) passes the level through unchanged
 * (SCPS280B Table 7-3, p.24).
 *
 * This never touches the output port register (0x01) or the
 * configuration register (0x03) -- it is safe to call regardless of a
 * pin's configured direction, because it only flips the bit a later
 * @ref tcal9538_get / @ref tcal9538_read_all reports, never anything
 * the chip drives. That makes it the one register on this part whose
 * effect is both real (a genuine round-trip through the chip, not just
 * an ACK) and observable on every pin without risking an output glitch
 * -- see the I/O-expander phase in examples/aen/aen-evk-demo for why
 * that property matters on a board where most pins are outputs a test
 * must never touch.
 *
 * @param ctx  TCAL9538 driver context (must be initialised first).
 * @param mask Bitmap, bit N = pin N's polarity-invert bit (1 = inverted).
 * @return ALP_OK, or ALP_ERR_NOT_READY if @p ctx is uninitialised.
 */
alp_status_t tcal9538_set_polarity_inversion(tcal9538_t *ctx, uint8_t mask);

/**
 * @brief Enable/disable per-pin input latching (register 0x42).
 *
 * TCAL9538-only.  Bit N of @p mask = 1 latches pin N's transition
 * until the input port register (0x00) is read via `tcal9538_get()`
 * or `tcal9538_read_all()`; bit N = 0 leaves the pin unlatched (the
 * interrupt still fires on a state change, but self-clears if the pin
 * returns to its prior level before that read -- SCPS280B p.25).
 *
 * @param ctx  TCAL9538 driver context (must be initialised first).
 * @param mask Bitmap, bit N = pin N's latch-enable bit.
 * @return ALP_OK, ALP_ERR_NOT_READY (uninitialised), or
 *         ALP_ERR_NOSUPPORT if @p ctx is not a TCAL9538 part (see
 *         `tcal9538_t::has_latched_irq`).
 */
alp_status_t tcal9538_set_input_latch(tcal9538_t *ctx, uint8_t mask);

/**
 * @brief Set the per-pin interrupt mask (register 0x45).
 *
 * TCAL9538-only.  Bit N of @p mask = 1 masks (disables) pin N's
 * contribution to \\INT; bit N = 0 enables it.  Power-up default is
 * 0xFF (every pin masked) -- a caller must clear the bits it wants
 * armed before an interrupt can assert (SCPS280B p.26).
 *
 * @param ctx  TCAL9538 driver context (must be initialised first).
 * @param mask Bitmap, bit N = pin N's mask bit (1 = masked).
 * @return ALP_OK, ALP_ERR_NOT_READY (uninitialised), or
 *         ALP_ERR_NOSUPPORT if @p ctx is not a TCAL9538 part (see
 *         `tcal9538_t::has_latched_irq`).
 */
alp_status_t tcal9538_set_interrupt_mask(tcal9538_t *ctx, uint8_t mask);

/**
 * @brief Read which pins are the current source of \\INT (register 0x46).
 *
 * TCAL9538-only.  Bit N of @p status_out = 1 means pin N is asserting
 * the interrupt (masked pins always read 0).  This register is
 * read-only and is NOT clear-on-read -- call `tcal9538_get()` or
 * `tcal9538_read_all()` (which read the input port register, 0x00)
 * afterwards to clear the condition and let \\INT de-assert
 * (SCPS280B p.26, Table 7-13).
 *
 * On the E1M EVK the expander's \\INT line shares the AUDIO_CLK pad
 * (see the file-level comment above), so a caller that cannot rely on
 * the edge pin while audio is active should poll this function
 * instead.
 *
 * @param[in]  ctx        TCAL9538 driver context (must be initialised first).
 * @param[out] status_out Interrupt-source bitmap (output; populated on success).
 * @return ALP_OK, ALP_ERR_NOT_READY (uninitialised), ALP_ERR_INVAL
 *         (NULL @p status_out), or ALP_ERR_NOSUPPORT if @p ctx is not
 *         a TCAL9538 part (see `tcal9538_t::has_latched_irq`).
 */
alp_status_t tcal9538_get_interrupt_status(tcal9538_t *ctx, uint8_t *status_out);

/**
 * @brief Configure a single pin's internal pull resistor (registers 0x43/0x44).
 *
 * TCAL9538-only.  Useful for the four sensor interrupt inputs on the
 * upper nibble when the driving sensor's output is open-drain rather
 * than push-pull.  Performs a read-modify-write of both registers.
 *
 * @param ctx  TCAL9538 driver context (must be initialised first).
 * @param pin  0..7
 * @param pull One of @ref tcal9538_pull_t.
 * @return ALP_OK, ALP_ERR_NOT_READY (uninitialised), ALP_ERR_INVAL
 *         (@p pin > 7), or ALP_ERR_NOSUPPORT if @p ctx is not a
 *         TCAL9538 part (see `tcal9538_t::has_latched_irq`).
 */
alp_status_t tcal9538_set_pull(tcal9538_t *ctx, uint8_t pin, tcal9538_pull_t pull);

/** @brief Release the driver context.  Idempotent. */
void tcal9538_deinit(tcal9538_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_TCAL9538_H */
