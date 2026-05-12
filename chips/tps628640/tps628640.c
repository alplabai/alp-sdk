/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * TI TPS628640 single-channel buck driver.  Register layout per the
 * TPS62864 / TPS62866 datasheet (SLVSEI1C, October 2020) -- the
 * TPS628640 / TPS628660 silicon and the TPS62864 / TPS62866 family
 * share one I2C register map, the part-number suffix selects the
 * default OTP voltage and the maximum output current.
 *
 * Register map (8-bit register addresses, 8-bit data, MSB-first I2C):
 *   0x01  VOUT1     R/W  output-voltage setpoint, register 1
 *   0x02  VOUT2     R/W  output-voltage setpoint, register 2 (VID-pin selected)
 *   0x03  CONTROL   W    operating-mode + ramp + reset
 *   0x05  STATUS    R    UVLO + HICCUP + thermal-warning latches
 *
 * VOUT encoding (8-bit unsigned, 5 mV step starting at 400 mV):
 *   register_byte = (mv - 400) / 5
 *   range: 0x00 (400 mV) .. 0xFF (1675 mV)
 *
 * The VID pin selects between VOUT1 (low) and VOUT2 (high) at
 * runtime; carriers that drive VID statically can pick either
 * register.  Carriers that hold VID at one level only use the
 * matching register.  This driver writes VOUT1 by default (the
 * V2N-M1 carrier holds VID low).  The raw register R/W helpers
 * remain available for callers that need direct access to VOUT2 /
 * CONTROL / STATUS.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/tps628640.h"

#define TPS628640_REG_VOUT1   0x01u
#define TPS628640_REG_VOUT2   0x02u
#define TPS628640_REG_CONTROL 0x03u
#define TPS628640_REG_STATUS  0x05u

/* VOUT register encoding constants. */
#define TPS628640_VOUT_BASE_MV   400u
#define TPS628640_VOUT_STEP_MV     5u
#define TPS628640_VOUT_MAX_MV   1675u

/* STATUS register bits (TPS62864 §8.6.6). */
#define TPS628640_STATUS_TWARN_BIT  (1u << 4)
#define TPS628640_STATUS_HICCUP_BIT (1u << 3)
#define TPS628640_STATUS_UVLO_BIT   (1u << 0)

static alp_status_t reg_read(tps628640_t *ctx, uint8_t reg, uint8_t *val) {
    return alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1u, val, 1u);
}

static alp_status_t reg_write(tps628640_t *ctx, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return alp_i2c_write(ctx->bus, ctx->addr, buf, sizeof(buf));
}

alp_status_t tps628640_init(tps628640_t *ctx, alp_i2c_t *bus,
                            uint8_t addr_7bit, uint16_t default_voltage_mv) {
    if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (addr_7bit > 0x7Fu) return ALP_ERR_INVAL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->bus                = bus;
    ctx->addr               = addr_7bit;
    ctx->default_voltage_mv = default_voltage_mv;

    /* ACK-probe via a read of VOUT1.  The TPS62864 family doesn't
     * carry a fixed device-id register, but every populated part
     * ACKs reads at any of the known register addresses, so a
     * successful read here is sufficient to confirm the chip is on
     * the bus at the expected address. */
    uint8_t      v = 0;
    alp_status_t s = reg_read(ctx, TPS628640_REG_VOUT1, &v);
    if (s != ALP_OK) return ALP_ERR_NOT_READY;

    ctx->initialised = true;
    return ALP_OK;
}

alp_status_t tps628640_set_voltage_mv(tps628640_t *ctx, uint16_t mv) {
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (mv < TPS628640_VOUT_BASE_MV || mv > TPS628640_VOUT_MAX_MV) {
        return ALP_ERR_OUT_OF_RANGE;
    }
    /* Encoding: byte = (mv - 400) / 5.  Round down on non-multiple
     * of 5 mV input; the caller can read back via _get_voltage_mv
     * to see what landed. */
    uint8_t reg = (uint8_t)((mv - TPS628640_VOUT_BASE_MV) / TPS628640_VOUT_STEP_MV);
    return reg_write(ctx, TPS628640_REG_VOUT1, reg);
}

alp_status_t tps628640_get_voltage_mv(tps628640_t *ctx, uint16_t *mv) {
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (mv == NULL) return ALP_ERR_INVAL;
    uint8_t      raw = 0;
    alp_status_t s   = reg_read(ctx, TPS628640_REG_VOUT1, &raw);
    if (s != ALP_OK) return s;
    *mv = (uint16_t)(TPS628640_VOUT_BASE_MV + (uint32_t)raw * TPS628640_VOUT_STEP_MV);
    return ALP_OK;
}

alp_status_t tps628640_get_status(tps628640_t *ctx, uint8_t *status_byte) {
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (status_byte == NULL) return ALP_ERR_INVAL;
    /* The STATUS register has read-and-clear semantics: every
     * latched bit is reset to 0 after a successful I2C read.
     * Callers MUST treat the returned byte as the snapshot of
     * everything that happened since the last read. */
    return reg_read(ctx, TPS628640_REG_STATUS, status_byte);
}

alp_status_t tps628640_read_reg(tps628640_t *ctx, uint8_t reg, uint8_t *val) {
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (val == NULL) return ALP_ERR_INVAL;
    return reg_read(ctx, reg, val);
}

alp_status_t tps628640_write_reg(tps628640_t *ctx, uint8_t reg, uint8_t val) {
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    return reg_write(ctx, reg, val);
}

void tps628640_deinit(tps628640_t *ctx) {
    if (ctx == NULL) return;
    ctx->initialised = false;
    ctx->bus         = NULL;
}
