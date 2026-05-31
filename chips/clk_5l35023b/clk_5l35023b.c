/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Renesas / IDT 5L35023(B) VersaClock 3S clock generator driver.
 * See <alp/chips/clk_5l35023b.h> for the register table cross-
 * reference and the V2N wiring notes.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/clk_5l35023b.h"

/* Byte 0x24 bit 7: I2C_PDB (chip power-down -- 0 = power down, 1 = normal). */
#define BYTE24_I2C_PDB_BIT (1u << 7)

alp_status_t clk_5l35023b_init(clk_5l35023b_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit)
{
    if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (addr_7bit > 0x7Fu) return ALP_ERR_INVAL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->bus  = bus;
    ctx->addr = addr_7bit;

    /* Cache Byte 0x00 (General Control).  Doubles as the ACK probe.
     * The bits[6:5] field is the I2C_addr[1:0] strap -- compare
     * against the address the caller asked for; if they don't agree
     * the chip is responding at the wrong slave.  Possible cause: a
     * mis-strapped board, or two chips sharing the bus at related
     * addresses. */
    uint8_t      reg = CLK_5L35023B_REG_GENERAL_CTRL;
    uint8_t      general_ctrl = 0u;
    alp_status_t s = alp_i2c_write_read(bus, addr_7bit, &reg, 1u,
                                        &general_ctrl, 1u);
    if (s != ALP_OK) return ALP_ERR_NOT_READY;

    const uint8_t strap_field   = (general_ctrl >> 5) & 0x3u;
    const uint8_t expected_addr = (uint8_t)(0x68u + strap_field);
    if (expected_addr != addr_7bit) return ALP_ERR_NOT_READY;

    /* Cache Byte 0x01 (Dash Code ID) so production-test code can
     * read the value without an extra I2C round-trip. */
    reg = CLK_5L35023B_REG_DASHCODE_ID;
    uint8_t dashcode = 0u;
    s = alp_i2c_write_read(bus, addr_7bit, &reg, 1u, &dashcode, 1u);
    if (s != ALP_OK) return ALP_ERR_NOT_READY;

    ctx->general_ctrl = general_ctrl;
    ctx->dashcode_id  = dashcode;
    ctx->initialised  = true;
    return ALP_OK;
}

alp_status_t clk_5l35023b_read_reg(clk_5l35023b_t *ctx, uint8_t reg, uint8_t *val)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (val == NULL) return ALP_ERR_INVAL;
    return alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1u, val, 1u);
}

alp_status_t clk_5l35023b_write_reg(clk_5l35023b_t *ctx, uint8_t reg, uint8_t val)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    uint8_t buf[2] = {reg, val};
    return alp_i2c_write(ctx->bus, ctx->addr, buf, sizeof(buf));
}

alp_status_t clk_5l35023b_register_dump(clk_5l35023b_t *ctx, uint8_t start_reg,
                                        uint8_t *out, size_t count)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (out == NULL || count == 0u) return ALP_ERR_INVAL;
    /* Datasheet: "data bytes are accessed in sequential order from
     * the lowest to the highest byte" -- a single write_read with
     * one register-address byte returns N consecutive registers. */
    return alp_i2c_write_read(ctx->bus, ctx->addr, &start_reg, 1u, out, count);
}

alp_status_t clk_5l35023b_read_dashcode_id(clk_5l35023b_t *ctx, uint8_t *dashcode)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (dashcode == NULL) return ALP_ERR_INVAL;
    uint8_t reg = CLK_5L35023B_REG_DASHCODE_ID;
    alp_status_t s = alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1u, dashcode, 1u);
    if (s == ALP_OK) ctx->dashcode_id = *dashcode;
    return s;
}

alp_status_t clk_5l35023b_get_strap_addr(clk_5l35023b_t *ctx,
                                         clk_5l35023b_strap_addr_t *strap)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (strap == NULL) return ALP_ERR_INVAL;
    *strap = (clk_5l35023b_strap_addr_t)((ctx->general_ctrl >> 5) & 0x3u);
    return ALP_OK;
}

alp_status_t clk_5l35023b_set_power_down(clk_5l35023b_t *ctx, bool powered_down)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;

    /* Read-modify-write Byte 0x24 -- preserves the SE1_CLKSEL1 /
     * REF_EN / DIV4_CH{2,3}_EN bits the board may have configured
     * for its specific output mix. */
    uint8_t reg = CLK_5L35023B_REG_SE1_DIV4_CTRL;
    uint8_t v   = 0u;
    alp_status_t s = alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1u, &v, 1u);
    if (s != ALP_OK) return s;

    /* I2C_PDB convention per datasheet: 0 = power down, 1 = normal. */
    if (powered_down) v &= (uint8_t)~BYTE24_I2C_PDB_BIT;
    else              v |= BYTE24_I2C_PDB_BIT;

    uint8_t buf[2] = {CLK_5L35023B_REG_SE1_DIV4_CTRL, v};
    return alp_i2c_write(ctx->bus, ctx->addr, buf, sizeof(buf));
}

void clk_5l35023b_deinit(clk_5l35023b_t *ctx)
{
    if (ctx == NULL) return;
    ctx->initialised = false;
    ctx->bus         = NULL;
}
