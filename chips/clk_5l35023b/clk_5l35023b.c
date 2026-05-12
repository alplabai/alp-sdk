/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Renesas / IDT 5L35023B clock generator driver (stub).  See
 * <alp/chips/clk_5l35023b.h> for the driver-status notes.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/clk_5l35023b.h"

alp_status_t clk_5l35023b_init(clk_5l35023b_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit)
{
    if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (addr_7bit > 0x7Fu) return ALP_ERR_INVAL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->bus  = bus;
    ctx->addr = addr_7bit;

    /* ACK-probe via a register-0 read.  The 5L35023B's reg 0 always
     * exists (datasheet TBD; the EVK Linux BSP confirms reg 0 is
     * the device-id / status register). */
    uint8_t      reg0 = 0u;
    uint8_t      ra   = 0x00u;
    alp_status_t s    = alp_i2c_write_read(bus, addr_7bit, &ra, 1u, &reg0, 1u);
    if (s != ALP_OK) return ALP_ERR_NOT_READY;

    ctx->initialised = true;
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
    /* 5L35023B auto-increments the register pointer after each
     * byte clocked out on the I2C read phase, so a single
     * write_read with a single reg-address byte produces N
     * consecutive registers. */
    return alp_i2c_write_read(ctx->bus, ctx->addr, &start_reg, 1u, out, count);
}

void clk_5l35023b_deinit(clk_5l35023b_t *ctx)
{
    if (ctx == NULL) return;
    ctx->initialised = false;
    ctx->bus         = NULL;
}
