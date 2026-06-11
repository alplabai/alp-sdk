/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Qorvo / Active-Semi ACT88760 primary PMIC driver.  See
 * <alp/chips/act8760.h> for the public API.  Register naming
 * follows the AA82BZ_RegisterMap_Users_Rev1P1 workbook (verified
 * cell-by-cell 2026-06-06) corroborated by ACT88760 Datasheet Rev C.
 * The V2N populates the ACT88760-120.E1 CMI on BRD_I2C addresses
 * 0x25 (ADD1) + 0x26 (ADD2).
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/act8760.h"

/* MSTR-tile system registers on the ADD1 slave.  Source of truth:
 * AA82BZ_RegisterMap_Users_Rev1P1 workbook, sheet MSTR (per-bit map),
 * corroborated by ACT88760 Datasheet Rev C -- extracted + adversarially
 * re-derived 2026-06-06.  Register 0x00 is the system status byte;
 * 0x01 carries the matching interrupt masks (TMSK = bit 5). */
#define ACT8760_REG_STATUS 0x00u
#define ACT8760_REG_TMASK 0x01u

/* Register 0x00 bit map, MSB->LSB (MSTR sheet, row 0x00):
 *   ROM_STAT | WD_TIMER_ALERT | TWARN | VSYSSTAT | VIN_POK_OV |
 *   PBASTAT | VSYSWARN | PBDSTAT
 * VSYSSTAT / VSYSWARN latch on the VIN falling edge, clear on read.
 * NOTE: SYSDAT (the raw VSYSMON sample) is register 0x02 bit 4, and
 * per-regulator ILIM / OV / POK flags live in each tile's offset-0
 * register -- neither is in this byte. */
#define ACT8760_STATUS_ROM_STAT 0x80u
#define ACT8760_STATUS_WD_ALERT 0x40u
#define ACT8760_STATUS_TWARN 0x20u
#define ACT8760_STATUS_VSYS_STAT 0x10u
#define ACT8760_STATUS_VIN_POK_OV 0x08u
#define ACT8760_STATUS_PBA_STAT 0x04u
#define ACT8760_STATUS_VSYS_WARN 0x02u
#define ACT8760_STATUS_PBD_STAT 0x01u

/* Per-rail VSET0 location.  Each regulator is a 0x20-wide register
 * tile: Buck1..6 at bases 0x40/0x60/0x80/0xA0/0xC0/0xE0 on ADD1;
 * Buck7 at base 0x00 on ADD2; the LDOs pair up in dual tiles on ADD2
 * (LDO12 @0x20, LDO53 @0x40, LDO64 @0x60 -- first LDO at +1, second
 * at +7, hence the LDO5/0x41-before-LDO3/0x47 ordering).  VSET0 sits
 * at buck-tile offset +2 / LDO slot offsets +1 / +7.
 * Bit 7 of every buck VSET byte is a live control bit (EN_OutPD or
 * IPD_SET) and the LDO bytes carry the RANGE bit above the 6-bit
 * field -- the accessors below mask reads and read-modify-write
 * writes accordingly.
 * Source: AA82BZ_RegisterMap_Users_Rev1P1, per-tile sheets (verified
 * cell-by-cell 2026-06-06; tile bases independently corroborated by
 * the MSTR 0x06 INTADR decode table). */
struct rail_loc {
    act8760_page_t page;
    uint8_t        vset0_reg; /* byte address of the VSET0 register */
    uint8_t        vset_mask; /* 0x7F (buck, 7-bit) or 0x3F (LDO, 6-bit) */
};

/* Order matches act8760_rail_t.  Slave assignment per tile layout:
 * ADD1 = MSTR + GPIO + Buck1..6 tiles; ADD2 = Buck7 + LDO1..6 tiles. */
static const struct rail_loc rail_table[ACT8760_RAIL_COUNT] = {
    [ACT8760_RAIL_BUCK1] = { ACT8760_PAGE_SYSTEM, 0x42, 0x7F },
    [ACT8760_RAIL_BUCK2] = { ACT8760_PAGE_SYSTEM, 0x62, 0x7F },
    [ACT8760_RAIL_BUCK3] = { ACT8760_PAGE_SYSTEM, 0x82, 0x7F },
    [ACT8760_RAIL_BUCK4] = { ACT8760_PAGE_SYSTEM, 0xA2, 0x7F },
    [ACT8760_RAIL_BUCK5] = { ACT8760_PAGE_SYSTEM, 0xC2, 0x7F },
    [ACT8760_RAIL_BUCK6] = { ACT8760_PAGE_SYSTEM, 0xE2, 0x7F },
    [ACT8760_RAIL_BUCK7] = { ACT8760_PAGE_AUX, 0x02, 0x7F },
    [ACT8760_RAIL_LDO1]  = { ACT8760_PAGE_AUX, 0x21, 0x3F },
    [ACT8760_RAIL_LDO2]  = { ACT8760_PAGE_AUX, 0x27, 0x3F },
    [ACT8760_RAIL_LDO3]  = { ACT8760_PAGE_AUX, 0x47, 0x3F },
    [ACT8760_RAIL_LDO4]  = { ACT8760_PAGE_AUX, 0x67, 0x3F },
    [ACT8760_RAIL_LDO5]  = { ACT8760_PAGE_AUX, 0x41, 0x3F },
    [ACT8760_RAIL_LDO6]  = { ACT8760_PAGE_AUX, 0x61, 0x3F },
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
    uint8_t buf[2] = { reg, val };
    return alp_i2c_write(ctx->bus, addr_for(ctx, page), buf, sizeof(buf));
}

alp_status_t act8760_init_at(act8760_t *ctx, alp_i2c_t *bus, uint8_t addr_page0)
{
    if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
    /* ADD2 always lives at ADD1 + 1; reject addresses that would
     * collide with reserved I2C addresses. */
    if (addr_page0 == 0x7F) return ALP_ERR_INVAL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->bus        = bus;
    ctx->addr_page0 = addr_page0;
    ctx->addr_page1 = (uint8_t)(addr_page0 + 1);

    /* Probe ADD1 by reading the system-status register (0x00, MSTR
     * tile) and ADD2 by reading register 0x00 of the Buck7 tile --
     * both are pure ACK checks; the returned value is ignored. */
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

    out->raw             = reg;
    out->rom_stat        = (reg & ACT8760_STATUS_ROM_STAT) != 0;
    out->wd_alert        = (reg & ACT8760_STATUS_WD_ALERT) != 0;
    out->thermal_warning = (reg & ACT8760_STATUS_TWARN) != 0;
    out->vsys_stat       = (reg & ACT8760_STATUS_VSYS_STAT) != 0;
    out->vin_pok_ov      = (reg & ACT8760_STATUS_VIN_POK_OV) != 0;
    out->pb_assert       = (reg & ACT8760_STATUS_PBA_STAT) != 0;
    out->vsys_warning    = (reg & ACT8760_STATUS_VSYS_WARN) != 0;
    out->pb_deassert     = (reg & ACT8760_STATUS_PBD_STAT) != 0;
    return ALP_OK;
}

alp_status_t act8760_read_reg(act8760_t *ctx, act8760_page_t page, uint8_t reg, uint8_t *out)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (out == NULL) return ALP_ERR_INVAL;
    if (page != ACT8760_PAGE_SYSTEM && page != ACT8760_PAGE_AUX) return ALP_ERR_INVAL;
    return reg_read(ctx, page, reg, out);
}

alp_status_t act8760_write_reg(act8760_t *ctx, act8760_page_t page, uint8_t reg, uint8_t val)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (page != ACT8760_PAGE_SYSTEM && page != ACT8760_PAGE_AUX) return ALP_ERR_INVAL;
    return reg_write(ctx, page, reg, val);
}

alp_status_t act8760_rail_get_vset(act8760_t *ctx, act8760_rail_t rail, uint8_t *vset_raw)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (vset_raw == NULL) return ALP_ERR_INVAL;
    if ((unsigned)rail >= ACT8760_RAIL_COUNT) return ALP_ERR_INVAL;

    const struct rail_loc *loc = &rail_table[rail];
    uint8_t                reg = 0;
    alp_status_t           s   = reg_read(ctx, loc->page, loc->vset0_reg, &reg);
    if (s != ALP_OK) return s;
    *vset_raw = reg & loc->vset_mask;
    return ALP_OK;
}

alp_status_t act8760_rail_set_vset(act8760_t *ctx, act8760_rail_t rail, uint8_t vset_raw)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if ((unsigned)rail >= ACT8760_RAIL_COUNT) return ALP_ERR_INVAL;
    const struct rail_loc *loc = &rail_table[rail];
    if ((vset_raw & ~loc->vset_mask) != 0) return ALP_ERR_INVAL;

    /* Read-modify-write: bit 7 (EN_OutPD / IPD_SET on bucks, RANGE
     * on LDO3..6) and bit 6 (RANGE on LDO1/2) are live configuration
     * bits sharing the VSET byte -- clobbering them would change the
     * rail's range or pull-down behaviour. */
    uint8_t      reg = 0;
    alp_status_t s   = reg_read(ctx, loc->page, loc->vset0_reg, &reg);
    if (s != ALP_OK) return s;
    reg = (uint8_t)((reg & ~loc->vset_mask) | vset_raw);
    return reg_write(ctx, loc->page, loc->vset0_reg, reg);
}

void act8760_deinit(act8760_t *ctx)
{
    if (ctx == NULL) return;
    ctx->initialised = false;
    ctx->bus         = NULL;
}
