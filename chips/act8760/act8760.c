/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Qorvo / Active-Semi ACT88760 primary PMIC driver.  See
 * <alp/chips/act8760.h> for the public API.  Register naming
 * follows the ACT88760 Datasheet Rev C; the V2N populates the
 * ACT88760-120.E1 CMI on BRD_I2C addresses 0x25 + 0x26.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/act8760.h"

/* System / status registers on page-0 (per datasheet section "I2C
 * Registers", references at lines 3096-3097 and 3823-3892 of the
 * extracted register description).  Each fault flag is read-to-clear:
 * the latched bit deasserts once the I2C controller reads register
 * 0x00, provided the fault condition itself has cleared. */
#define ACT8760_REG_STATUS  0x00u  /* SYSDAT | SYSSTAT | TWARN | etc.   */
#define ACT8760_REG_TMASK   0x01u  /* bit 5 = TMSK (thermal mask)        */
#define ACT8760_REG_GPIO_STAT_LO 0x03u
#define ACT8760_REG_OV_UV_CFG 0x09u /* DisOvUvShdn lives here.            */

/* Status-byte bit assignments per ACT88760 datasheet table for
 * register 0x00 (system status word).  Bits not yet decoded are
 * passed through in act8760_status_t.raw for application use. */
#define ACT8760_STATUS_TWARN     0x80u
#define ACT8760_STATUS_SYSWARN   0x40u
#define ACT8760_STATUS_SYSDAT    0x20u
#define ACT8760_STATUS_ILIM_WARN 0x10u
#define ACT8760_STATUS_FAULT_ANY 0x08u

/* VSET-register byte addresses.  The datasheet does not name them
 * uniformly per regulator (Buck1's VSET0 is the "B1_VSET0" bit field
 * inside a wider register; the offsets below come from the Users
 * Guide Rev 3.0 page 12 register-summary table and match the values
 * the Qorvo TileLib XMLs emit).  Marked TODO where the offset still
 * needs cross-checking against an authoritative source. */
struct rail_loc {
    act8760_page_t page;
    uint8_t        vset0_reg; /* offset of the VSET0 byte */
    uint8_t        vset_mask; /* 0x7F (buck, 7-bit) or 0x3F (LDO, 6-bit) */
};

/* Order matches act8760_rail_t.  Page assignment per Table 1 of the
 * datasheet: page0 = Buck1..6 + GPIOs + system; page1 = Buck7 + LDOs.
 *
 * The vset0_reg offsets below are TODO -- cross-reference against the
 * ACT88760 Users Guide Rev 3.0 page-12 register summary or the
 * BUCK<n>_19Feb2025 TileLib XML before relying on them in production.
 * The driver's probe/status surface is exercised at runtime; the VSET
 * accessors return ALP_ERR_NOSUPPORT until the offsets are confirmed. */
static const struct rail_loc rail_table[ACT8760_RAIL_COUNT] = {
    [ACT8760_RAIL_BUCK1] = {ACT8760_PAGE_SYSTEM, 0x10, 0x7F}, /* TODO: confirm */
    [ACT8760_RAIL_BUCK2] = {ACT8760_PAGE_SYSTEM, 0x14, 0x7F}, /* TODO: confirm */
    [ACT8760_RAIL_BUCK3] = {ACT8760_PAGE_SYSTEM, 0x18, 0x7F}, /* TODO: confirm */
    [ACT8760_RAIL_BUCK4] = {ACT8760_PAGE_SYSTEM, 0x1C, 0x7F}, /* TODO: confirm */
    [ACT8760_RAIL_BUCK5] = {ACT8760_PAGE_SYSTEM, 0x20, 0x7F}, /* TODO: confirm */
    [ACT8760_RAIL_BUCK6] = {ACT8760_PAGE_SYSTEM, 0x24, 0x7F}, /* TODO: confirm */
    [ACT8760_RAIL_BUCK7] = {ACT8760_PAGE_AUX,    0x10, 0x7F}, /* TODO: confirm */
    [ACT8760_RAIL_LDO1]  = {ACT8760_PAGE_AUX,    0x20, 0x3F}, /* TODO: confirm */
    [ACT8760_RAIL_LDO2]  = {ACT8760_PAGE_AUX,    0x24, 0x3F}, /* TODO: confirm */
    [ACT8760_RAIL_LDO3]  = {ACT8760_PAGE_AUX,    0x28, 0x3F}, /* TODO: confirm */
    [ACT8760_RAIL_LDO4]  = {ACT8760_PAGE_AUX,    0x2C, 0x3F}, /* TODO: confirm */
    [ACT8760_RAIL_LDO5]  = {ACT8760_PAGE_AUX,    0x30, 0x3F}, /* TODO: confirm */
    [ACT8760_RAIL_LDO6]  = {ACT8760_PAGE_AUX,    0x34, 0x3F}, /* TODO: confirm */
};

static uint8_t addr_for(const act8760_t *ctx, act8760_page_t page)
{
    return page == ACT8760_PAGE_SYSTEM ? ctx->addr_page0 : ctx->addr_page1;
}

static alp_status_t reg_read(act8760_t *ctx, act8760_page_t page, uint8_t reg, uint8_t *out)
{
    return alp_i2c_write_read(ctx->bus, addr_for(ctx, page), &reg, 1, out, 1);
}

static alp_status_t reg_write(act8760_t *ctx, act8760_page_t page, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return alp_i2c_write(ctx->bus, addr_for(ctx, page), buf, sizeof(buf));
}

alp_status_t act8760_init_at(act8760_t *ctx, alp_i2c_t *bus, uint8_t addr_page0)
{
    if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
    /* Page-1 always lives at page0 + 1; reject pairs that would
     * collide with reserved I2C addresses. */
    if (addr_page0 == 0x7F) return ALP_ERR_INVAL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->bus        = bus;
    ctx->addr_page0 = addr_page0;
    ctx->addr_page1 = (uint8_t)(addr_page0 + 1);

    /* Probe by reading register 0x00 on both pages.  An ACK on page-0
     * confirms the chip is on the bus; an ACK on page-1 confirms the
     * CMI exposes the second slave address (every CMI does, but
     * pages-1 ACK is the cleanest separate signal). */
    uint8_t tmp = 0;
    if (reg_read(ctx, ACT8760_PAGE_SYSTEM, ACT8760_REG_STATUS, &tmp) != ALP_OK) {
        return ALP_ERR_NOT_READY;
    }
    if (reg_read(ctx, ACT8760_PAGE_AUX, 0x00, &tmp) != ALP_OK) {
        return ALP_ERR_NOT_READY;
    }

    ctx->initialised = true;
    return ALP_OK;
}

alp_status_t act8760_init(act8760_t *ctx, alp_i2c_t *bus)
{
    return act8760_init_at(ctx, bus, ACT8760_I2C_ADDR_PAGE0);
}

alp_status_t act8760_get_status(act8760_t *ctx, act8760_status_t *out)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (out == NULL) return ALP_ERR_INVAL;

    uint8_t      reg = 0;
    alp_status_t s   = reg_read(ctx, ACT8760_PAGE_SYSTEM, ACT8760_REG_STATUS, &reg);
    if (s != ALP_OK) return s;

    out->raw              = reg;
    out->thermal_warning  = (reg & ACT8760_STATUS_TWARN) != 0;
    out->sys_warning      = (reg & ACT8760_STATUS_SYSWARN) != 0;
    out->sys_data         = (reg & ACT8760_STATUS_SYSDAT) != 0;
    out->ilim_warning     = (reg & ACT8760_STATUS_ILIM_WARN) != 0;
    out->fault_pending    = (reg & ACT8760_STATUS_FAULT_ANY) != 0;
    return ALP_OK;
}

alp_status_t act8760_read_reg(act8760_t *ctx, act8760_page_t page,
                              uint8_t reg, uint8_t *out)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (out == NULL) return ALP_ERR_INVAL;
    if (page != ACT8760_PAGE_SYSTEM && page != ACT8760_PAGE_AUX) return ALP_ERR_INVAL;
    return reg_read(ctx, page, reg, out);
}

alp_status_t act8760_write_reg(act8760_t *ctx, act8760_page_t page,
                               uint8_t reg, uint8_t val)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (page != ACT8760_PAGE_SYSTEM && page != ACT8760_PAGE_AUX) return ALP_ERR_INVAL;
    return reg_write(ctx, page, reg, val);
}

alp_status_t act8760_rail_get_vset(act8760_t *ctx, act8760_rail_t rail,
                                   uint8_t *vset_raw)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (vset_raw == NULL) return ALP_ERR_INVAL;
    if ((unsigned)rail >= ACT8760_RAIL_COUNT) return ALP_ERR_INVAL;

    /* Register offsets in rail_table[] still need cross-check vs the
     * Users Guide Rev 3.0 register-summary table -- guard with an
     * explicit NOSUPPORT until the offsets are confirmed and the
     * TODOs in this file are cleared. */
    return ALP_ERR_NOSUPPORT;
}

alp_status_t act8760_rail_set_vset(act8760_t *ctx, act8760_rail_t rail,
                                   uint8_t vset_raw)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if ((unsigned)rail >= ACT8760_RAIL_COUNT) return ALP_ERR_INVAL;
    const struct rail_loc *loc = &rail_table[rail];
    if ((vset_raw & ~loc->vset_mask) != 0) return ALP_ERR_INVAL;
    /* Same TODO gate as the get -- VSET offset table needs confirmation. */
    return ALP_ERR_NOSUPPORT;
}

void act8760_deinit(act8760_t *ctx)
{
    if (ctx == NULL) return;
    ctx->initialised = false;
    ctx->bus         = NULL;
}
