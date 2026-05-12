/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * TI TPS628640 single-channel buck driver -- STUB.  See
 * <alp/chips/tps628640.h> for status and rationale.  When the TI
 * datasheet is added to the vendor datasheet, the
 * voltage / status helpers here need their register-layout
 * implementations.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/tps628640.h"

alp_status_t tps628640_init(tps628640_t *ctx, alp_i2c_t *bus,
                            uint8_t addr_7bit, uint16_t default_voltage_mv)
{
    if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (addr_7bit > 0x7F) return ALP_ERR_INVAL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->bus                = bus;
    ctx->addr               = addr_7bit;
    ctx->default_voltage_mv = default_voltage_mv;

    /* ACK-probe: issue a zero-length write.  Some TI parts NAK that;
     * if so the loop drops down to a single-byte register read on a
     * register address that the part is known to ACK on -- this stub
     * cannot pick that register without the datasheet, so a NAK on
     * the zero-length probe falls back to ALP_ERR_NOT_READY. */
    uint8_t      dummy = 0;
    alp_status_t s     = alp_i2c_write(ctx->bus, ctx->addr, &dummy, 0);
    if (s != ALP_OK) return ALP_ERR_NOT_READY;

    ctx->initialised = true;
    return ALP_OK;
}

alp_status_t tps628640_set_voltage_mv(tps628640_t *ctx, uint16_t mv)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    (void)mv;
    /* TODO: implement VSET register write once the TI datasheet for
     * TPS628640 is added to the vendor datasheet.  Must reject
     * voltages outside the rail's safe-operating window (see header). */
    return ALP_ERR_NOSUPPORT;
}

alp_status_t tps628640_get_voltage_mv(tps628640_t *ctx, uint16_t *mv)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (mv == NULL) return ALP_ERR_INVAL;
    /* TODO: implement VSET register read decode. */
    return ALP_ERR_NOSUPPORT;
}

alp_status_t tps628640_get_status(tps628640_t *ctx, uint8_t *status_byte)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (status_byte == NULL) return ALP_ERR_INVAL;
    /* TODO: identify the chip's status register and surface its bits. */
    return ALP_ERR_NOSUPPORT;
}

alp_status_t tps628640_read_reg(tps628640_t *ctx, uint8_t reg, uint8_t *val)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (val == NULL) return ALP_ERR_INVAL;
    return alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, val, 1);
}

alp_status_t tps628640_write_reg(tps628640_t *ctx, uint8_t reg, uint8_t val)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    uint8_t buf[2] = {reg, val};
    return alp_i2c_write(ctx->bus, ctx->addr, buf, sizeof(buf));
}

void tps628640_deinit(tps628640_t *ctx)
{
    if (ctx == NULL) return;
    ctx->initialised = false;
    ctx->bus         = NULL;
}
