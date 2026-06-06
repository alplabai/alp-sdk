/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * TI TCA9538 / TCAL9538 8-channel I2C I/O expander driver.
 * See <alp/chips/tcal9538.h> for the public API and register map.
 */

#include <string.h>

#include "alp/chips/tcal9538.h"

#define TCAL9538_REG_INPUT 0x00u
#define TCAL9538_REG_OUTPUT 0x01u
#define TCAL9538_REG_POL 0x02u
#define TCAL9538_REG_CFG 0x03u

static alp_status_t reg_read(tcal9538_t *ctx, uint8_t reg, uint8_t *val_out)
{
    return alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, val_out, 1);
}

static alp_status_t reg_write(tcal9538_t *ctx, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return alp_i2c_write(ctx->bus, ctx->addr, buf, sizeof(buf));
}

alp_status_t tcal9538_init(tcal9538_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit)
{
    if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->bus  = bus;
    ctx->addr = (addr_7bit == 0) ? TCAL9538_I2C_ADDR_BASE : addr_7bit;

    /* Probe + read back the configuration / output state so the
     * cached values stay coherent if the chip was already in a
     * non-default state (e.g. left configured by an earlier boot). */
    alp_status_t s = reg_read(ctx, TCAL9538_REG_CFG, &ctx->cfg_cache);
    if (s != ALP_OK) return ALP_ERR_NOT_READY;
    s = reg_read(ctx, TCAL9538_REG_OUTPUT, &ctx->out_cache);
    if (s != ALP_OK) return s;

    ctx->initialised = true;
    return ALP_OK;
}

alp_status_t tcal9538_set_direction(tcal9538_t *ctx, uint8_t pin, tcal9538_direction_t dir)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (pin > 7) return ALP_ERR_INVAL;
    uint8_t bit = (uint8_t)(1u << pin);
    uint8_t cfg = ctx->cfg_cache;
    if (dir == TCAL9538_DIR_INPUT) {
        cfg |= bit;
    } else {
        cfg &= (uint8_t)~bit;
    }
    if (cfg == ctx->cfg_cache) return ALP_OK;
    alp_status_t s = reg_write(ctx, TCAL9538_REG_CFG, cfg);
    if (s == ALP_OK) ctx->cfg_cache = cfg;
    return s;
}

alp_status_t tcal9538_set_directions(tcal9538_t *ctx, uint8_t mask, uint8_t value)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    uint8_t cfg = (uint8_t)((ctx->cfg_cache & ~mask) | (value & mask));
    if (cfg == ctx->cfg_cache) return ALP_OK;
    alp_status_t s = reg_write(ctx, TCAL9538_REG_CFG, cfg);
    if (s == ALP_OK) ctx->cfg_cache = cfg;
    return s;
}

alp_status_t tcal9538_set(tcal9538_t *ctx, uint8_t pin, bool level)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (pin > 7) return ALP_ERR_INVAL;
    uint8_t bit = (uint8_t)(1u << pin);
    uint8_t out = ctx->out_cache;
    if (level)
        out |= bit;
    else
        out &= (uint8_t)~bit;
    if (out == ctx->out_cache) return ALP_OK;
    alp_status_t s = reg_write(ctx, TCAL9538_REG_OUTPUT, out);
    if (s == ALP_OK) ctx->out_cache = out;
    return s;
}

alp_status_t tcal9538_get(tcal9538_t *ctx, uint8_t pin, bool *level_out)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (pin > 7 || level_out == NULL) return ALP_ERR_INVAL;
    uint8_t      port = 0;
    alp_status_t s    = reg_read(ctx, TCAL9538_REG_INPUT, &port);
    if (s != ALP_OK) return s;
    *level_out = (port & (1u << pin)) != 0;
    return ALP_OK;
}

alp_status_t tcal9538_read_all(tcal9538_t *ctx, uint8_t *port_out)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (port_out == NULL) return ALP_ERR_INVAL;
    return reg_read(ctx, TCAL9538_REG_INPUT, port_out);
}

alp_status_t tcal9538_write_all(tcal9538_t *ctx, uint8_t port)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    alp_status_t s = reg_write(ctx, TCAL9538_REG_OUTPUT, port);
    if (s == ALP_OK) ctx->out_cache = port;
    return s;
}

void tcal9538_deinit(tcal9538_t *ctx)
{
    if (ctx == NULL) return;
    ctx->initialised = false;
    ctx->bus         = NULL;
}
