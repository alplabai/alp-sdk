/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file clk_5l35023b.h
 * @brief Renesas / IDT 5L35023B audio-rate clock generator driver (stub).
 *
 * The 5L35023B is a low-jitter I2C-programmable clock generator
 * originally from IDT (now Renesas Electronics post-2019 acquisition).
 * On the V2N module it acts as the **`Audio_CLKB`** source feeding
 * the on-board audio rail; the OE pin is wired to Renesas `P67`
 * (`Audio_CLKB_OE`) so the host can gate the clock.
 *
 * @par Driver status: STUB
 *
 * No 5L35023B datasheet is currently in the vendor
 * documentation archive.  The Renesas RZ/V2N EVK reference
 * design uses the same part and likely carries a register-init
 * sequence somewhere in its BSP (TBD: maintainer to mirror the
 * relevant init blob from the Renesas Linux BSP repo and drop the
 * datasheet alongside).  Until that lands, this driver is a thin
 * stub:
 *
 *   - `clk_5l35023b_init`         -- probe via a register-0 read.
 *   - `clk_5l35023b_read_reg`     -- raw register read.
 *   - `clk_5l35023b_write_reg`    -- raw register write.
 *   - `clk_5l35023b_register_dump` -- read N consecutive registers
 *                                    into a caller buffer, useful
 *                                    for production-test logging.
 *
 * Application of an authoritative output-frequency config (PLL
 * dividers, drive strength, OE polarity) is **out of scope** until
 * the datasheet / Renesas BSP blob is in hand.  Carriers should
 * treat the chip as running its factory-default config out of POR;
 * the host gates the output via the carrier-controlled
 * `Audio_CLKB_OE` GPIO (no I2C write required).
 *
 * @par Carrier wiring (V2N)
 *
 * | Signal         | Renesas pad | Notes                                       |
 * |----------------|-------------|---------------------------------------------|
 * | Audio_CLKB     | P10         | clock output to E1M AR9 AUDIO_CLK           |
 * | Audio_CLKB_OE  | P67         | output-enable strap (active-high)           |
 * | I2C            | BRD_I2C     | management bus; default 7-bit address 0x68  |
 * |                |             | (= 8-bit write 0xD0 per Renesas datasheet)  |
 *
 * @par Datasheet provenance
 * - Renesas 5L35023 datasheet (public; see
 *   <https://www.renesas.com/en/document/dst/5l35023-datasheet>).
 *   Upgrade `driver_status` from `stub` to `partial` / `complete`
 *   in `metadata/chips/clk_5l35023b.yaml` once the EVK init sequence
 *   is mirrored into the driver.
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

/** Default 7-bit I2C slave address per the Renesas 5L35023
 *  datasheet (8-bit write address = `0xD0`). */
#define CLK_5L35023B_I2C_ADDR_DEFAULT 0x68u

/** Driver context. */
typedef struct {
    bool       initialised;
    alp_i2c_t *bus;
    uint8_t    addr;       /**< 7-bit I2C slave address. */
} clk_5l35023b_t;

/**
 * @brief Probe the chip at @p addr.  Reads register 0 to confirm an ACK.
 *
 * @param ctx        Driver context (output).
 * @param bus        BRD_I2C handle.
 * @param addr_7bit  7-bit slave address.  Maintainer-supplied (TBD
 *                   per `metadata/e1m_modules/E1M-V2N101/som.yaml`).
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY (no ACK).
 */
alp_status_t clk_5l35023b_init(clk_5l35023b_t *ctx, alp_i2c_t *bus,
                               uint8_t addr_7bit);

/** @brief Raw register read. */
alp_status_t clk_5l35023b_read_reg(clk_5l35023b_t *ctx, uint8_t reg, uint8_t *val);

/** @brief Raw register write. */
alp_status_t clk_5l35023b_write_reg(clk_5l35023b_t *ctx, uint8_t reg, uint8_t val);

/**
 * @brief Sequentially read @p count registers starting at @p start_reg.
 *
 * Useful for production-test logging (dump the chip's full register
 * file for QC comparison against the factory-known-good state).
 *
 * @param start_reg  First register to read.
 * @param out        Destination buffer; must hold at least @p count bytes.
 * @param count      Number of consecutive registers (>= 1).
 */
alp_status_t clk_5l35023b_register_dump(clk_5l35023b_t *ctx, uint8_t start_reg,
                                        uint8_t *out, size_t count);

/** @brief Release the context.  Idempotent. */
void         clk_5l35023b_deinit(clk_5l35023b_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CLK_5L35023B_H */
