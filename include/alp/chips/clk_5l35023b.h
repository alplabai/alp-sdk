/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file clk_5l35023b.h
 * @brief Renesas / IDT 5L35023(B) VersaClock 3S programmable clock generator.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Three-PLL 1.8 V clock generator with OTP-loadable configuration:
 * up to six unique frequency outputs (2 differential pairs + 3
 * single-ended), 1 MHz..125 MHz per output, with PCIe Gen 1/2/3
 * jitter compliance.  On the V2N module the chip acts as the
 * **`Audio_CLKB`** source feeding the on-board audio rail; the OE
 * pin is wired to Renesas `P67` (`Audio_CLKB_OE`) so the host can
 * gate the output without touching I2C.
 *
 * @par Driver status: PARTIAL
 *
 * Lifecycle + raw register R/W + register dump + typed Dash-Code-ID
 * read + soft power-down toggle.  Application of a full PLL / OUTDIV
 * config (PLL1/2/3 M+N dividers, OUTDIVn source, spread-spectrum,
 * VDD rail selection) is left to a higher layer because V2N's
 * design intent is to rely on the chip's OTP defaults + the
 * board-driven `Audio_CLKB_OE` GPIO.  Boards that need to
 * reprogram on the fly compose the raw R/W helpers against the
 * datasheet's register table (cross-referenced below).
 *
 * @par Board wiring (V2N)
 *
 * | Signal         | Renesas pad | Notes                                       |
 * |----------------|-------------|---------------------------------------------|
 * | `Audio_CLKB`   | `P10`       | clock output to E1M AR9 AUDIO_CLK           |
 * | `Audio_CLKB_OE`| `P67`       | output-enable strap (active-high)           |
 * | I2C            | `BRD_I2C`   | management bus; default 7-bit address 0x68  |
 * |                |             | (= 8-bit write 0xD0 per the Renesas datasheet) |
 *
 * @par I2C slave addressing
 *
 * Per the Renesas 5L35023 datasheet (December 2025 rev) the chip
 * presents one of four 7-bit slave addresses selected by the
 * `I2C_addr[1:0]` strap (Byte `0x00` bits[6:5]).  The strap state
 * survives reset and is read-back through I2C:
 *
 * | `I2C_addr[1:0]` | 7-bit | 8-bit write | 8-bit read |
 * |-----------------|-------|-------------|------------|
 * | `00`            | 0x68  | 0xD0        | 0xD1       |
 * | `01`            | 0x69  | 0xD2        | 0xD3       |
 * | `10`            | 0x6A  | 0xD4        | 0xD5       |
 * | `11`            | 0x6B  | 0xD6        | 0xD7       |
 *
 * V2N straps `I2C_addr[1:0] = 00` -> 7-bit `0x68`.
 *
 * @par Register-table cross-reference (Renesas 5L35023 datasheet)
 *
 * | Reg    | Name                  | Purpose                                                   |
 * |--------|-----------------------|-----------------------------------------------------------|
 * | `0x00` | General Control       | OTP burned/protect; I2C addr strap readback; PLL1 SS en   |
 * | `0x01` | Dash Code ID          | 8-bit factory-stamped configuration identifier            |
 * | `0x02` | Crystal Cap Setting   | 8-bit xtal load-trim                                      |
 * | `0x03`..`0x06` | PLL3 dividers + CP                                                        |
 * | `0x07`..`0x0E` | PLL1 M+N+CP + OUTDIV5                                                     |
 * | `0x0F`..`0x1C` | PLL2 + OUTDIV1..4 + spread-spectrum                                       |
 * | `0x1D` | Output Control        | DIFF1/2 enable, OUTDIV4 source, SE1 slew, VDD1 sel        |
 * | `0x1E` | OE / DFC Control      | SE1/3 enable, OE1/OE3 pin function                        |
 * | `0x1F` | Control               | SE2 freerun, VDD2 sel, PLL2 CH2 enable                    |
 * | `0x20` | Control               | SE2 enable, OE2 pin function, DFC enable                  |
 * | `0x21`..`0x23` | SE3 / DIFF1 / DIFF2 output mode + slew                                    |
 * | `0x24` | SE1 + DIV4 + power-dn | `I2C_PDB` chip soft power-down (bit 7), REF/DIV4 enable   |
 *
 * Higher-numbered registers cover the chip's diagnostic + DFC seeds
 * (Bytes `0x25`..`0x47` per the datasheet) and are not yet wrapped
 * by the typed helpers below; reach them via the raw R/W path.
 *
 * @par Datasheet provenance
 * - Renesas / IDT **5L35023 Datasheet** (public).  Document URL:
 *   <https://www.renesas.com/en/document/dst/5l35023-datasheet>.
 *   Register table extracted into the cross-reference above.
 */

#ifndef ALP_CHIPS_CLK_5L35023B_H
#define ALP_CHIPS_CLK_5L35023B_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default 7-bit I2C slave address (8-bit write `0xD0`).  Matches
 *  the V2N strap `I2C_addr[1:0] = 00`. */
#define CLK_5L35023B_I2C_ADDR_DEFAULT 0x68u

/** Documented register offsets the typed helpers operate on.
 *  Customers can reach any other register via the raw R/W helpers. */
#define CLK_5L35023B_REG_GENERAL_CTRL  0x00u /**< Byte 0 */
#define CLK_5L35023B_REG_DASHCODE_ID   0x01u /**< Byte 1 */
#define CLK_5L35023B_REG_OUTPUT_CTRL   0x1Du /**< Byte 29: DIFF1/2 enable */
#define CLK_5L35023B_REG_OE_DFC_CTRL   0x1Eu /**< Byte 30: SE1/3 enable */
#define CLK_5L35023B_REG_DIFF1_CTRL    0x22u /**< Byte 34: DIFF1 mode + slew */
#define CLK_5L35023B_REG_SE1_DIV4_CTRL 0x24u /**< Byte 36: I2C_PDB (bit 7) */

/** Decoded `I2C_addr[1:0]` strap from Byte 0x00 bits[6:5]. */
typedef enum {
	CLK_5L35023B_STRAP_ADDR_0X68 = 0, /**< Bits[6:5] = 00. */
	CLK_5L35023B_STRAP_ADDR_0X69 = 1, /**< Bits[6:5] = 01. */
	CLK_5L35023B_STRAP_ADDR_0X6A = 2, /**< Bits[6:5] = 10. */
	CLK_5L35023B_STRAP_ADDR_0X6B = 3, /**< Bits[6:5] = 11. */
} clk_5l35023b_strap_addr_t;

/** Driver context. */
typedef struct {
	bool       initialised;
	alp_i2c_t *bus;
	uint8_t    addr;         /**< 7-bit I2C slave address. */
	uint8_t    dashcode_id;  /**< Cached Byte 0x01 read at init. */
	uint8_t    general_ctrl; /**< Cached Byte 0x00 read at init. */
} clk_5l35023b_t;

/**
 * @brief Probe the chip at @p addr_7bit, read Byte 0x00 (General
 *        Control) and Byte 0x01 (Dash Code ID), and cache both
 *        into the context for diagnostic readback.
 *
 * The init read of Byte 0x00 doubles as a sanity check: the chip's
 * `I2C_addr[1:0]` bits in the byte must match the address the caller
 * passed in (i.e. the strap and the call agree).  If they don't,
 * init returns @ref ALP_ERR_NOT_READY -- the chip is responding at
 * an address the schematic didn't intend.
 *
 * @param ctx        Driver context (output).
 * @param bus        BRD_I2C handle.
 * @param addr_7bit  7-bit slave address (0x68..0x6B per the
 *                   `I2C_addr[1:0]` strap).
 *
 * @return @ref ALP_OK / @ref ALP_ERR_INVAL / @ref ALP_ERR_NOT_READY.
 */
alp_status_t clk_5l35023b_init(clk_5l35023b_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit);

/** @brief Raw register read.  Operates on any documented register. */
alp_status_t clk_5l35023b_read_reg(clk_5l35023b_t *ctx, uint8_t reg, uint8_t *val);

/** @brief Raw register write.  Writes to OTP-shadow registers take
 *         effect immediately; they revert to the OTP defaults on the
 *         next power-cycle unless the OTP itself is burned (out of
 *         scope for this driver -- requires the Renesas / IDT
 *         programming flow on `OE1` with `VDDDIFF1` raised to 6.5 V). */
alp_status_t clk_5l35023b_write_reg(clk_5l35023b_t *ctx, uint8_t reg, uint8_t val);

/**
 * @brief Sequentially read @p count registers starting at @p start_reg.
 *
 * Useful for production-test logging (dump the chip's full register
 * file for QC comparison against the factory-known-good state).
 * Datasheet documents the protocol as "data bytes are accessed in
 * sequential order from the lowest to the highest byte" so a single
 * write_read with one register-address byte returns N consecutive
 * registers.
 */
alp_status_t
clk_5l35023b_register_dump(clk_5l35023b_t *ctx, uint8_t start_reg, uint8_t *out, size_t count);

/**
 * @brief Read the factory Dash-Code-ID (Byte 0x01).
 *
 * The Dash-Code-ID is the 8-bit configuration identifier Renesas
 * stamps into the chip's OTP at burn time -- a part marked
 * `5L35023B-042NLGI` carries dash code `0x2A` (decimal 42).
 * Production test uses this to confirm the chip received the
 * expected configuration profile.
 *
 * The init helper caches the value into @ref clk_5l35023b_t::dashcode_id;
 * call this helper to re-read on demand.
 */
alp_status_t clk_5l35023b_read_dashcode_id(clk_5l35023b_t *ctx, uint8_t *dashcode);

/**
 * @brief Decode the chip's strapped I2C address from the cached
 *        Byte 0x00 contents (bits[6:5]).
 *
 * @return @ref ALP_OK / @ref ALP_ERR_NOT_READY (driver uninitialised).
 */
alp_status_t clk_5l35023b_get_strap_addr(clk_5l35023b_t *ctx, clk_5l35023b_strap_addr_t *strap);

/**
 * @brief Toggle the chip's soft power-down state (`I2C_PDB` -- Byte
 *        0x24 bit 7).
 *
 * Per the datasheet: `I2C_PDB = 0` -> chip power-down (stops all
 * outputs except the always-on 32 kHz RTC pin); `I2C_PDB = 1` ->
 * normal operation (default after POR).
 *
 * Read-modify-write internally so other bits in Byte 0x24
 * (`SE1_CLKSEL1`, `REF_EN`, `DIV4_CH3_EN`, `DIV4_CH2_EN`) are
 * preserved.
 */
alp_status_t clk_5l35023b_set_power_down(clk_5l35023b_t *ctx, bool powered_down);

/** @brief Release the context.  Idempotent. */
void clk_5l35023b_deinit(clk_5l35023b_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CLK_5L35023B_H */
