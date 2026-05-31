/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * TI TMP112 digital temperature sensor driver.
 * See <alp/chips/tmp112.h> for the public API and datasheet pin
 * map.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/tmp112.h"

#define TMP112_REG_TEMP 0x00u
#define TMP112_REG_CONF 0x01u
#define TMP112_REG_T_LOW 0x02u
#define TMP112_REG_T_HIGH 0x03u

/* CONF bit fields (see datasheet table 9, big-endian on the wire). */
#define TMP112_CONF_OS 0x8000u  /* One-shot                  */
#define TMP112_CONF_R1 0x4000u  /* Resolution bit 1 (read only, always 1) */
#define TMP112_CONF_R0 0x2000u  /* Resolution bit 0 (read only, always 1) */
#define TMP112_CONF_F1 0x1000u  /* Fault queue 1             */
#define TMP112_CONF_F0 0x0800u  /* Fault queue 0             */
#define TMP112_CONF_POL 0x0400u /* Alert polarity            */
#define TMP112_CONF_TM 0x0200u  /* Thermostat mode           */
#define TMP112_CONF_SD 0x0100u  /* Shutdown                  */
#define TMP112_CONF_CR1 0x0080u /* Conversion rate bit 1     */
#define TMP112_CONF_CR0 0x0040u /* Conversion rate bit 0     */
#define TMP112_CONF_AL 0x0020u  /* Alert (read-only)         */
#define TMP112_CONF_EM 0x0010u  /* Extended mode (13-bit)    */

/* Default CONF on power-on per datasheet: 0x60A0 (continuous, 4 Hz). */
#define TMP112_CONF_DEFAULT 0x60A0u

static alp_status_t tmp112_read_reg16(tmp112_t *ctx, uint8_t reg, uint16_t *val_out)
{
    uint8_t      buf[2] = {0};
    alp_status_t s      = alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, buf, sizeof(buf));
    if (s != ALP_OK) return s;
    *val_out = ((uint16_t)buf[0] << 8) | buf[1];
    return ALP_OK;
}

static alp_status_t tmp112_write_reg16(tmp112_t *ctx, uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = {reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)};
    return alp_i2c_write(ctx->bus, ctx->addr, buf, sizeof(buf));
}

alp_status_t tmp112_init(tmp112_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit)
{
    if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (addr_7bit < TMP112_I2C_ADDR_GND || addr_7bit > TMP112_I2C_ADDR_SCL) {
        return ALP_ERR_INVAL;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->bus  = bus;
    ctx->addr = addr_7bit;

    /* Probe via CONF read -- the upper byte's R1:R0 bits must read 11. */
    uint16_t     conf = 0;
    alp_status_t s    = tmp112_read_reg16(ctx, TMP112_REG_CONF, &conf);
    if (s != ALP_OK) return s;
    if ((conf & (TMP112_CONF_R1 | TMP112_CONF_R0)) != (TMP112_CONF_R1 | TMP112_CONF_R0)) {
        return ALP_ERR_NOT_READY; /* Not a TMP112 -- probably mis-addressed */
    }

    s = tmp112_write_reg16(ctx, TMP112_REG_CONF, TMP112_CONF_DEFAULT);
    if (s != ALP_OK) return s;
    ctx->extended_mode = false;
    ctx->initialised   = true;
    return ALP_OK;
}

alp_status_t tmp112_set_rate(tmp112_t *ctx, tmp112_rate_t rate)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    uint16_t     conf = 0;
    alp_status_t s    = tmp112_read_reg16(ctx, TMP112_REG_CONF, &conf);
    if (s != ALP_OK) return s;
    conf &= ~(TMP112_CONF_CR1 | TMP112_CONF_CR0);
    conf |= ((uint16_t)rate & 0x3) << 6;
    return tmp112_write_reg16(ctx, TMP112_REG_CONF, conf);
}

alp_status_t tmp112_set_extended_mode(tmp112_t *ctx, bool extended)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    uint16_t     conf = 0;
    alp_status_t s    = tmp112_read_reg16(ctx, TMP112_REG_CONF, &conf);
    if (s != ALP_OK) return s;
    if (extended)
        conf |= TMP112_CONF_EM;
    else
        conf &= ~TMP112_CONF_EM;
    s = tmp112_write_reg16(ctx, TMP112_REG_CONF, conf);
    if (s == ALP_OK) ctx->extended_mode = extended;
    return s;
}

alp_status_t tmp112_read_temp_milli_c(tmp112_t *ctx, int32_t *temp_milli_c)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (temp_milli_c == NULL) return ALP_ERR_INVAL;

    uint16_t     raw = 0;
    alp_status_t s   = tmp112_read_reg16(ctx, TMP112_REG_TEMP, &raw);
    if (s != ALP_OK) return s;

    /* TMP112 packs the 12-bit (or 13-bit in extended mode) value
     * left-justified.  Sign-extend before the divide. */
    int32_t shifted;
    if (ctx->extended_mode) {
        /* 13-bit, right-shift by 3, sign-extend. */
        int32_t sx = (int16_t)raw;
        sx >>= 3;
        shifted = sx;
    } else {
        /* 12-bit, right-shift by 4, sign-extend. */
        int32_t sx = (int16_t)raw;
        sx >>= 4;
        shifted = sx;
    }
    /* Each LSB = 0.0625 C = 62.5 milli-C.  Multiply by 625, then
     * divide by 10 -- avoids the half-LSB (62.5) by keeping integer
     * arithmetic. */
    *temp_milli_c = (shifted * 625) / 10;
    return ALP_OK;
}

void tmp112_deinit(tmp112_t *ctx)
{
    if (ctx == NULL) return;
    ctx->initialised = false;
    ctx->bus         = NULL;
}
