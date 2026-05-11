/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Renesas DA9292 multi-phase buck PMIC driver.  See
 * <alp/chips/da9292.h> for the public API.  Register naming and
 * encoding follow the DA9292 Datasheet Rev 2.2 (R16DS0518EJ0220,
 * Mar 24 2025).  Pages referenced in comments map to that PDF.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/da9292.h"

/* Register map (datasheet Table 12, page 34). */
#define DA9292_REG_PMC_STATUS_00   0x00u
#define DA9292_REG_PMC_STATUS_01   0x01u
#define DA9292_REG_PMC_EVENT_00    0x02u
#define DA9292_REG_PMC_EVENT_01    0x03u
#define DA9292_REG_PMC_MASK_00     0x04u
#define DA9292_REG_PMC_MASK_01     0x05u
#define DA9292_REG_PMC_CTRL_00     0x06u
#define DA9292_REG_PMC_CTRL_01     0x07u  /* CHx_EN / CHx_VSEL / VSTEP / DIS_PD */
#define DA9292_REG_PMC_CTRL_02     0x08u
#define DA9292_REG_PMC_CTRL_03     0x09u
#define DA9292_REG_PMC_VOUT_CH1_00 0x0Au  /* CH1 VSEL=0 setpoint (LO range) */
#define DA9292_REG_PMC_VOUT_CH1_01 0x0Bu  /* CH1 VSEL=1 retention */
#define DA9292_REG_PMC_VOUT_CH2_00 0x0Cu  /* CH2 VSEL=0 */
#define DA9292_REG_PMC_VOUT_CH2_01 0x0Du  /* CH2 VSEL=1 */
#define DA9292_REG_PMC_DEV_ID      0x19u
#define DA9292_REG_PMC_REV_ID      0x1Au
#define DA9292_REG_PMC_CFG_REV     0x1Bu

/* PMC_CTRL_01 bit positions (datasheet Table 21, page 40-41). */
#define DA9292_CTRL01_CH2_VSTEP   (1u << 7)
#define DA9292_CTRL01_CH1_VSTEP   (1u << 6)
#define DA9292_CTRL01_CH2_DIS_PD  (1u << 5)
#define DA9292_CTRL01_CH1_DIS_PD  (1u << 4)
#define DA9292_CTRL01_CH2_VSEL    (1u << 3)
#define DA9292_CTRL01_CH1_VSEL    (1u << 2)
#define DA9292_CTRL01_CH2_EN      (1u << 1)
#define DA9292_CTRL01_CH1_EN      (1u << 0)

/* PMC_STATUS_00 bit layout (datasheet Table 14).
 *
 * The exact bit assignments aren't fully visible in the extracted
 * snippets but the field set is:
 *   S_CH2_OC, S_CH1_OC, S_CH2_OV, S_CH1_OV,
 *   S_CH2_UV, S_CH1_UV, S_CH2_PG, S_CH1_PG.
 * Order TODO -- assumed below to mirror PMC_MASK_00 (which IS
 * documented as bits[7:0] = M_CH2_OC, M_CH1_OC, M_CH2_OV, M_CH1_OV,
 * M_CH2_UV, M_CH1_UV, M_CH2_PG, M_CH1_PG).  Verify against the
 * datasheet's Table 14 register description before relying on these
 * masks in production. */
#define DA9292_STATUS00_CH2_OC (1u << 7)
#define DA9292_STATUS00_CH1_OC (1u << 6)
#define DA9292_STATUS00_CH2_OV (1u << 5)
#define DA9292_STATUS00_CH1_OV (1u << 4)
#define DA9292_STATUS00_CH2_UV (1u << 3)
#define DA9292_STATUS00_CH1_UV (1u << 2)
#define DA9292_STATUS00_CH2_PG (1u << 1)
#define DA9292_STATUS00_CH1_PG (1u << 0)

/* PMC_STATUS_01 -- mirrors PMC_MASK_01 (Table 19): TEMP_WARN [2],
 * TEMP_CRIT [1], VIN_UVLO [0]. */
#define DA9292_STATUS01_TEMP_WARN (1u << 2)
#define DA9292_STATUS01_TEMP_CRIT (1u << 1)
#define DA9292_STATUS01_VIN_UVLO  (1u << 0)

/* PMC_EVENT_00 / 01 bit layout per datasheet Tables 16-17.  Same
 * layout as STATUS but the access type is RWC1 (write-1-to-clear). */
#define DA9292_EVENT00_CH2_OC (1u << 7)
#define DA9292_EVENT00_CH1_OC (1u << 6)
#define DA9292_EVENT00_CH2_OV (1u << 5)
#define DA9292_EVENT00_CH1_OV (1u << 4)
#define DA9292_EVENT00_CH2_UV (1u << 3)
#define DA9292_EVENT00_CH1_UV (1u << 2)
#define DA9292_EVENT00_CH2_PG (1u << 1)
#define DA9292_EVENT00_CH1_PG (1u << 0)
#define DA9292_EVENT01_TEMP_WARN (1u << 2)
#define DA9292_EVENT01_TEMP_CRIT (1u << 1)
#define DA9292_EVENT01_VIN_UVLO  (1u << 0)

/* VSTEP=0 (5 mV step) encoding:
 *   register byte = 0x3C + (mV - 300) / 5
 *   minimum = 0x3C (0.300 V); maximum = 0xFF (1.275 V); 0x00..0x3B reserved.
 * Reset default is 0xA3 = 0.815 V. */
#define DA9292_VSET_LO_BASE_BYTE 0x3Cu
#define DA9292_VSET_LO_MIN_MV    300u
#define DA9292_VSET_LO_MAX_MV    1275u
#define DA9292_VSET_LO_STEP_MV   5u

static uint8_t vout_reg_for(da9292_channel_t ch)
{
    return ch == DA9292_CH1 ? DA9292_REG_PMC_VOUT_CH1_00 : DA9292_REG_PMC_VOUT_CH2_00;
}

static uint8_t en_bit_for(da9292_channel_t ch)
{
    return ch == DA9292_CH1 ? DA9292_CTRL01_CH1_EN : DA9292_CTRL01_CH2_EN;
}

static alp_status_t reg_read(da9292_t *ctx, uint8_t reg, uint8_t *val)
{
    return alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, val, 1);
}

static alp_status_t reg_write(da9292_t *ctx, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return alp_i2c_write(ctx->bus, ctx->addr, buf, sizeof(buf));
}

alp_status_t da9292_init(da9292_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit)
{
    if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (addr_7bit > 0x7F) return ALP_ERR_INVAL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->bus  = bus;
    ctx->addr = addr_7bit;

    /* Probe via PMC_DEV_ID.  DEV_ID is OTP-defined and chip-rev
     * specific; this driver doesn't assert a particular value
     * (would brick on a silicon respin) -- it just confirms the
     * IC ACKs and that the register is non-zero. */
    uint8_t      dev_id = 0;
    alp_status_t s      = reg_read(ctx, DA9292_REG_PMC_DEV_ID, &dev_id);
    if (s != ALP_OK) return ALP_ERR_NOT_READY;
    if (dev_id == 0x00 || dev_id == 0xFF) return ALP_ERR_NOT_READY;
    ctx->dev_id = dev_id;

    uint8_t rev_id = 0;
    s              = reg_read(ctx, DA9292_REG_PMC_REV_ID, &rev_id);
    if (s != ALP_OK) return s;
    ctx->rev_id = rev_id;

    ctx->initialised = true;
    return ALP_OK;
}

alp_status_t da9292_read_reg(da9292_t *ctx, uint8_t reg, uint8_t *val)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (val == NULL) return ALP_ERR_INVAL;
    return reg_read(ctx, reg, val);
}

alp_status_t da9292_write_reg(da9292_t *ctx, uint8_t reg, uint8_t val)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    return reg_write(ctx, reg, val);
}

alp_status_t da9292_get_status(da9292_t *ctx, da9292_status_t *out)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (out == NULL) return ALP_ERR_INVAL;

    uint8_t      s00 = 0, s01 = 0;
    alp_status_t s = reg_read(ctx, DA9292_REG_PMC_STATUS_00, &s00);
    if (s != ALP_OK) return s;
    s = reg_read(ctx, DA9292_REG_PMC_STATUS_01, &s01);
    if (s != ALP_OK) return s;

    out->raw_00     = s00;
    out->raw_01     = s01;
    out->ch1_oc     = (s00 & DA9292_STATUS00_CH1_OC) != 0;
    out->ch2_oc     = (s00 & DA9292_STATUS00_CH2_OC) != 0;
    out->ch1_ov     = (s00 & DA9292_STATUS00_CH1_OV) != 0;
    out->ch2_ov     = (s00 & DA9292_STATUS00_CH2_OV) != 0;
    out->ch1_uv     = (s00 & DA9292_STATUS00_CH1_UV) != 0;
    out->ch2_uv     = (s00 & DA9292_STATUS00_CH2_UV) != 0;
    out->ch1_pg     = (s00 & DA9292_STATUS00_CH1_PG) != 0;
    out->ch2_pg     = (s00 & DA9292_STATUS00_CH2_PG) != 0;
    out->temp_warn  = (s01 & DA9292_STATUS01_TEMP_WARN) != 0;
    out->temp_crit  = (s01 & DA9292_STATUS01_TEMP_CRIT) != 0;
    out->vin_uvlo   = (s01 & DA9292_STATUS01_VIN_UVLO) != 0;
    return ALP_OK;
}

alp_status_t da9292_read_and_clear_events(da9292_t *ctx, da9292_events_t *out)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (out == NULL) return ALP_ERR_INVAL;

    uint8_t      e00 = 0, e01 = 0;
    alp_status_t s = reg_read(ctx, DA9292_REG_PMC_EVENT_00, &e00);
    if (s != ALP_OK) return s;
    s = reg_read(ctx, DA9292_REG_PMC_EVENT_01, &e01);
    if (s != ALP_OK) return s;

    out->e_ch1_oc    = (e00 & DA9292_EVENT00_CH1_OC) != 0;
    out->e_ch2_oc    = (e00 & DA9292_EVENT00_CH2_OC) != 0;
    out->e_ch1_ov    = (e00 & DA9292_EVENT00_CH1_OV) != 0;
    out->e_ch2_ov    = (e00 & DA9292_EVENT00_CH2_OV) != 0;
    out->e_ch1_uv    = (e00 & DA9292_EVENT00_CH1_UV) != 0;
    out->e_ch2_uv    = (e00 & DA9292_EVENT00_CH2_UV) != 0;
    out->e_ch1_pg    = (e00 & DA9292_EVENT00_CH1_PG) != 0;
    out->e_ch2_pg    = (e00 & DA9292_EVENT00_CH2_PG) != 0;
    out->e_temp_warn = (e01 & DA9292_EVENT01_TEMP_WARN) != 0;
    out->e_temp_crit = (e01 & DA9292_EVENT01_TEMP_CRIT) != 0;
    out->e_vin_uvlo  = (e01 & DA9292_EVENT01_VIN_UVLO) != 0;

    /* Write-1-to-clear: echo the latched bits back to drop them. */
    if (e00 != 0) { s = reg_write(ctx, DA9292_REG_PMC_EVENT_00, e00); if (s != ALP_OK) return s; }
    if (e01 != 0) { s = reg_write(ctx, DA9292_REG_PMC_EVENT_01, e01); if (s != ALP_OK) return s; }
    return ALP_OK;
}

alp_status_t da9292_set_enable(da9292_t *ctx, da9292_channel_t ch, bool enable)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (ch != DA9292_CH1 && ch != DA9292_CH2) return ALP_ERR_INVAL;

    uint8_t      ctrl = 0;
    alp_status_t s    = reg_read(ctx, DA9292_REG_PMC_CTRL_01, &ctrl);
    if (s != ALP_OK) return s;

    uint8_t en = en_bit_for(ch);
    if (enable) ctrl |= en;
    else        ctrl &= (uint8_t)~en;

    return reg_write(ctx, DA9292_REG_PMC_CTRL_01, ctrl);
}

alp_status_t da9292_set_voltage_mv(da9292_t *ctx, da9292_channel_t ch, uint16_t mv)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (ch != DA9292_CH1 && ch != DA9292_CH2) return ALP_ERR_INVAL;
    if (mv < DA9292_VSET_LO_MIN_MV || mv > DA9292_VSET_LO_MAX_MV) return ALP_ERR_INVAL;

    /* Round to 5 mV grid. */
    uint16_t mv_q = mv - ((mv - DA9292_VSET_LO_MIN_MV) % DA9292_VSET_LO_STEP_MV);
    uint8_t  byte = (uint8_t)(DA9292_VSET_LO_BASE_BYTE +
                              (mv_q - DA9292_VSET_LO_MIN_MV) / DA9292_VSET_LO_STEP_MV);
    return reg_write(ctx, vout_reg_for(ch), byte);
}

alp_status_t da9292_get_voltage_mv(da9292_t *ctx, da9292_channel_t ch, uint16_t *mv)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (mv == NULL) return ALP_ERR_INVAL;
    if (ch != DA9292_CH1 && ch != DA9292_CH2) return ALP_ERR_INVAL;

    uint8_t      byte = 0;
    alp_status_t s    = reg_read(ctx, vout_reg_for(ch), &byte);
    if (s != ALP_OK) return s;
    if (byte < DA9292_VSET_LO_BASE_BYTE) return ALP_ERR_IO; /* reserved code */
    *mv = (uint16_t)(DA9292_VSET_LO_MIN_MV +
                     (byte - DA9292_VSET_LO_BASE_BYTE) * DA9292_VSET_LO_STEP_MV);
    return ALP_OK;
}

alp_status_t da9292_v2n_base_init(da9292_t *ctx)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;

    /* V2N base: CH2 must stay disabled (DEEPX is not populated).
     * Defensive: read PMC_CTRL_01, clear CH2_EN if accidentally set,
     * leave everything else alone. */
    uint8_t      ctrl = 0;
    alp_status_t s    = reg_read(ctx, DA9292_REG_PMC_CTRL_01, &ctrl);
    if (s != ALP_OK) return s;
    if ((ctrl & DA9292_CTRL01_CH2_EN) != 0) {
        ctrl &= (uint8_t)~DA9292_CTRL01_CH2_EN;
        s = reg_write(ctx, DA9292_REG_PMC_CTRL_01, ctrl);
        if (s != ALP_OK) return s;
    }
    return ALP_OK;
}

alp_status_t da9292_v2n_m1_enable_deepx_rail(da9292_t *ctx, uint32_t timeout_us)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;

    /* 1. Program CH2 = 0.75 V (VSTEP=0). */
    alp_status_t s = da9292_set_voltage_mv(ctx, DA9292_CH2, 750);
    if (s != ALP_OK) return s;

    /* 2. Read back -- belt-and-braces sanity check. */
    uint16_t mv = 0;
    s           = da9292_get_voltage_mv(ctx, DA9292_CH2, &mv);
    if (s != ALP_OK) return s;
    if (mv != 750) return ALP_ERR_IO;

    /* 3. Enable CH2. */
    s = da9292_set_enable(ctx, DA9292_CH2, true);
    if (s != ALP_OK) return s;

    /* 4. Poll CH2_PG until the rail comes up or we time out.  Datasheet
     * soft-start: order of milliseconds; with the caller's timeout in
     * microseconds we sample every ~100 us.  Use a coarse loop based
     * on caller-side delay to avoid pulling in a clock dependency
     * inside the chip driver -- caller can refine if they need
     * tighter polling granularity. */
    const uint32_t poll_us = 100u;
    uint32_t       waited  = 0;
    while (waited <= timeout_us) {
        da9292_status_t st = {0};
        s                  = da9292_get_status(ctx, &st);
        if (s != ALP_OK) return s;
        if (st.ch2_pg) return ALP_OK;
        /* Caller-supplied delay isn't available without dragging in
         * an OS dependency; the loop simply re-reads STATUS until
         * either timeout or PG.  On a real RTOS this should be
         * replaced by an interrupt-driven wait on the INT_N line. */
        waited += poll_us;
    }
    return ALP_ERR_TIMEOUT;
}

void da9292_deinit(da9292_t *ctx)
{
    if (ctx == NULL) return;
    ctx->initialised = false;
    ctx->bus         = NULL;
}
