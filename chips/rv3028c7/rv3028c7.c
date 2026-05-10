/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Micro Crystal RV-3028-C7 RTC driver.  See <alp/chips/rv3028c7.h>
 * for the public API.  Register map per RV-3028-C7
 * Application Manual v1.4.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/rv3028c7.h"

#define RV3028_REG_SECONDS 0x00
#define RV3028_REG_MINUTES 0x01
#define RV3028_REG_HOURS 0x02
#define RV3028_REG_WEEKDAY 0x03
#define RV3028_REG_DATE 0x04
#define RV3028_REG_MONTH 0x05
#define RV3028_REG_YEAR 0x06
#define RV3028_REG_ALARM_MIN 0x07
#define RV3028_REG_ALARM_HR 0x08
#define RV3028_REG_ALARM_WD 0x09
#define RV3028_REG_STATUS 0x0E
#define RV3028_REG_CONTROL_1 0x0F
#define RV3028_REG_CONTROL_2 0x10

#define RV3028_STATUS_AF 0x04   /* Alarm flag (write 0 to clear) */
#define RV3028_STATUS_PORF 0x01 /* Power-on reset flag           */

#define RV3028_CTRL2_AIE 0x08 /* Alarm interrupt enable       */
#define RV3028_CTRL2_24H 0x40 /* 24-hour mode (1 = 24h)       */

/* Alarm enable bits live in the alarm registers themselves (top
 * bit "AE" of each MIN/HR/WD register; 0 = participates in match,
 * 1 = ignored). */
#define RV3028_ALARM_AE 0x80

static uint8_t bcd_to_bin(uint8_t bcd)
{
    return (bcd & 0x0F) + ((bcd >> 4) & 0x0F) * 10;
}
static uint8_t bin_to_bcd(uint8_t bin)
{
    return (uint8_t)(((bin / 10) << 4) | (bin % 10));
}

static alp_status_t rv3028_read(rv3028c7_t *ctx, uint8_t reg, uint8_t *buf, size_t len)
{
    return alp_i2c_write_read(ctx->bus, RV3028C7_I2C_ADDR, &reg, 1, buf, len);
}
static alp_status_t rv3028_write(rv3028c7_t *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t scratch[16];
    if (len + 1 > sizeof(scratch)) return ALP_ERR_INVAL;
    scratch[0] = reg;
    memcpy(scratch + 1, data, len);
    return alp_i2c_write(ctx->bus, RV3028C7_I2C_ADDR, scratch, len + 1);
}

static alp_status_t rv3028_write_reg(rv3028c7_t *ctx, uint8_t reg, uint8_t val)
{
    return rv3028_write(ctx, reg, &val, 1);
}

alp_status_t rv3028c7_init(rv3028c7_t *ctx, alp_i2c_t *bus)
{
    if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->bus = bus;

    /* Probe by reading STATUS; any I2C error -> NOT_READY. */
    uint8_t      status = 0;
    alp_status_t s      = rv3028_read(ctx, RV3028_REG_STATUS, &status, 1);
    if (s != ALP_OK) return ALP_ERR_NOT_READY;

    /* Clear PORF if present.  Apps that care about "is the RTC
     * holding a valid time?" should check PORF before calling
     * init -- they can read it via rv3028_read_status() once the
     * v0.3.x diagnostic helpers land. */
    if (status & RV3028_STATUS_PORF) {
        (void)rv3028_write_reg(ctx, RV3028_REG_STATUS, status & ~RV3028_STATUS_PORF);
    }

    /* Force 24-hour mode (bit 6 of CONTROL_2). */
    uint8_t ctrl2 = 0;
    s             = rv3028_read(ctx, RV3028_REG_CONTROL_2, &ctrl2, 1);
    if (s != ALP_OK) return s;
    ctrl2 |= RV3028_CTRL2_24H;
    s = rv3028_write_reg(ctx, RV3028_REG_CONTROL_2, ctrl2);
    if (s != ALP_OK) return s;

    ctx->initialised = true;
    return ALP_OK;
}

alp_status_t rv3028c7_get_time(rv3028c7_t *ctx, rv3028c7_time_t *out)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (out == NULL) return ALP_ERR_INVAL;

    uint8_t      buf[7] = {0};
    alp_status_t s      = rv3028_read(ctx, RV3028_REG_SECONDS, buf, sizeof(buf));
    if (s != ALP_OK) return s;

    out->second  = bcd_to_bin(buf[0] & 0x7F);
    out->minute  = bcd_to_bin(buf[1] & 0x7F);
    out->hour    = bcd_to_bin(buf[2] & 0x3F);
    out->weekday = (uint8_t)(buf[3] & 0x07);
    out->day     = bcd_to_bin(buf[4] & 0x3F);
    out->month   = bcd_to_bin(buf[5] & 0x1F);
    out->year    = (uint16_t)(2000 + bcd_to_bin(buf[6] & 0xFF));
    return ALP_OK;
}

alp_status_t rv3028c7_set_time(rv3028c7_t *ctx, const rv3028c7_time_t *t)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (t == NULL) return ALP_ERR_INVAL;
    if (t->second > 59 || t->minute > 59 || t->hour > 23 || t->weekday < 1 || t->weekday > 7 ||
        t->day < 1 || t->day > 31 || t->month < 1 || t->month > 12 || t->year < 2000 ||
        t->year > 2099) {
        return ALP_ERR_INVAL;
    }

    uint8_t buf[7];
    buf[0] = bin_to_bcd(t->second);
    buf[1] = bin_to_bcd(t->minute);
    buf[2] = bin_to_bcd(t->hour);
    buf[3] = (uint8_t)(t->weekday & 0x07);
    buf[4] = bin_to_bcd(t->day);
    buf[5] = bin_to_bcd(t->month);
    buf[6] = bin_to_bcd((uint8_t)(t->year - 2000));
    return rv3028_write(ctx, RV3028_REG_SECONDS, buf, sizeof(buf));
}

alp_status_t rv3028c7_set_alarm(rv3028c7_t *ctx, const rv3028c7_time_t *when,
                                const rv3028c7_alarm_match_t *match)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (when == NULL || match == NULL) return ALP_ERR_INVAL;

    uint8_t buf[3];
    /* AE bit = 1 disables that field's match comparison. */
    buf[0] = bin_to_bcd(when->minute) | (match->match_minute ? 0 : RV3028_ALARM_AE);
    buf[1] = bin_to_bcd(when->hour) | (match->match_hour ? 0 : RV3028_ALARM_AE);
    if (match->use_weekday) {
        buf[2] = (when->weekday & 0x07) | (match->match_day_or_weekday ? 0 : RV3028_ALARM_AE);
    } else {
        buf[2] = bin_to_bcd(when->day) | (match->match_day_or_weekday ? 0 : RV3028_ALARM_AE);
    }
    /* Set / clear WADA in CONTROL_1 to pick weekday-or-date semantics. */
    uint8_t      ctrl1 = 0;
    alp_status_t s     = rv3028_read(ctx, RV3028_REG_CONTROL_1, &ctrl1, 1);
    if (s != ALP_OK) return s;
    if (match->use_weekday)
        ctrl1 &= ~0x20;
    else
        ctrl1 |= 0x20;
    s = rv3028_write_reg(ctx, RV3028_REG_CONTROL_1, ctrl1);
    if (s != ALP_OK) return s;

    return rv3028_write(ctx, RV3028_REG_ALARM_MIN, buf, sizeof(buf));
}

alp_status_t rv3028c7_alarm_int_enable(rv3028c7_t *ctx, bool enable)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    uint8_t      ctrl2 = 0;
    alp_status_t s     = rv3028_read(ctx, RV3028_REG_CONTROL_2, &ctrl2, 1);
    if (s != ALP_OK) return s;
    if (enable)
        ctrl2 |= RV3028_CTRL2_AIE;
    else
        ctrl2 &= ~RV3028_CTRL2_AIE;
    return rv3028_write_reg(ctx, RV3028_REG_CONTROL_2, ctrl2);
}

alp_status_t rv3028c7_alarm_check_and_clear(rv3028c7_t *ctx, bool *fired)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (fired == NULL) return ALP_ERR_INVAL;
    uint8_t      status = 0;
    alp_status_t s      = rv3028_read(ctx, RV3028_REG_STATUS, &status, 1);
    if (s != ALP_OK) return s;
    *fired = (status & RV3028_STATUS_AF) != 0;
    if (*fired) {
        s = rv3028_write_reg(ctx, RV3028_REG_STATUS, status & ~RV3028_STATUS_AF);
    }
    return s;
}

void rv3028c7_deinit(rv3028c7_t *ctx)
{
    if (ctx == NULL) return;
    ctx->initialised = false;
    ctx->bus         = NULL;
}
