/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Qorvo / Active-Semi ACT88760 primary PMIC driver.  See
 * <alp/chips/act8760.h> for the public API.  Register naming
 * follows the AA82BZ_RegisterMap_Users_Rev1P1 workbook (verified
 * cell-by-cell 2026-06-06, re-read 2026-09-24 for the tile ON / range /
 * status bits and the MSTR GPIO map) corroborated by ACT88760
 * Datasheet Rev C.  The V2N populates the ACT88760-120.E1 CMI on
 * BRD_I2C addresses 0x25 (ADD1) + 0x26 (ADD2).
 *
 * Every write path is guarded: nothing is written until a limits table
 * is installed (act8760_set_limits), and even then only through a typed,
 * window- / critical- / mask-checked API or the raw write allow-list.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/act8760.h"

/* MSTR-tile system registers on the ADD1 slave (workbook sheet MSTR). */
#define ACT8760_REG_STATUS      0x00u /* system status; VSYS latches clear on read */
#define ACT8760_REG_GPIO_STAT   0x03u /* GPIO8..1 real-time level, bit7..0 */
#define ACT8760_REG_GPIO_TOGGLE 0x04u /* GPIO8..1 toggle detect, read clears */
#define ACT8760_REG_GPIO_MASK   0x05u /* GPIO8..1 input-toggle IRQ mask */
#define ACT8760_REG_MODE1       0x0Du /* MODE1..MODE7 = 0x0D..0x13 */
#define ACT8760_REG_MODE8       0x27u /* MODE8..MODE11 = 0x27..0x2A */
#define ACT8760_REG_GPIO9_11    0x2Bu /* GPIO9..11 STAT / toggle / mask */
#define ACT8760_REG_DVS_SEL     0x2Cu /* bit1 GPIO11 MASK, bit0 BAND_SEL */
#define ACT8760_REG_PUSH_PULL   0x34u /* EN_PUSH_PULL GPIO8,7,6,4,3,2,1 */

#define ACT8760_MODE_POLARITY 0x80u /* MODEx bit7: polarity */
#define ACT8760_MODE_MUX      0x0Fu /* MODEx bits3:0: function MUX */
#define ACT8760_BAND_SEL      0x01u /* 0x2C bit0: 1 = VSET2/3 aliased onto VSET0/1 */
#define ACT8760_GPIO11_MASK   0x02u /* 0x2C bit1 */
#define ACT8760_TILE_ON       0x80u /* bit7 of every tile's ON register */

/* Register 0x00 bit map, MSB->LSB (MSTR sheet, row 0x00):
 *   ROM_STAT | WD_TIMER_ALERT | TWARN | VSYSSTAT | VIN_POK_OV |
 *   PBASTAT | VSYSWARN | PBDSTAT */
#define ACT8760_STATUS_ROM_STAT   0x80u
#define ACT8760_STATUS_WD_ALERT   0x40u
#define ACT8760_STATUS_TWARN      0x20u
#define ACT8760_STATUS_VSYS_STAT  0x10u
#define ACT8760_STATUS_VIN_POK_OV 0x08u
#define ACT8760_STATUS_PBA_STAT   0x04u
#define ACT8760_STATUS_VSYS_WARN  0x02u
#define ACT8760_STATUS_PBD_STAT   0x01u

/* Tile status byte (offset 0 of a buck tile / LDO slot). */
#define ACT8760_TILE_POK       0x80u
#define ACT8760_TILE_OV        0x40u
#define ACT8760_TILE_ILIM      0x20u
#define ACT8760_TILE_ILIM_WARN 0x10u /* bucks only; RFU on LDOs */

/* VOUT = base_mv + VSET * step_uv / 1000, per range. */
struct vscale {
	uint16_t base_mv;
	uint16_t step_uv;
};

/* Per-rail register layout.  Each regulator is a 0x20-wide tile:
 * Buck1..6 at 0x40/0x60/0x80/0xA0/0xC0/0xE0 on ADD1; Buck7 at 0x00 on
 * ADD2; the LDOs pair up in dual tiles on ADD2 (LDO12 @0x20, LDO53
 * @0x40, LDO64 @0x60 -- first LDO slot at +0, second at +6).
 *
 * Range selector per the workbook:
 *   - Buck1/2/7: tile +6 bit1 (Vout_Range).
 *   - Buck3/4: tile +1 bit3 (Vout_Range; +6 bits1:0 there are DBSTBY).
 *   - Buck5/6: Output-High (25 mV) only.
 *   - LDO1/2: VSET byte bit6; LDO3..6: VSET byte bit7. */
struct rail_loc {
	act8760_page_t page;
	uint8_t        status_reg;
	uint8_t        vset_reg;    /* VSET0 */
	uint8_t        en_reg;      /* ON bit7 */
	uint8_t        range_reg;   /* valid when range_mask != 0 */
	uint8_t        range_mask;  /* 0 = no user range bit, see fixed_range */
	uint8_t        fixed_range; /* used when range_mask == 0 */
	uint8_t        vset_mask;   /* 0x7F buck, 0x3F LDO */
	bool           is_buck;
	bool           banked;   /* Buck1/2/7: VSET0 bank-aliased by BAND_SEL */
	struct vscale  scale[2]; /* index = range */
};

#define BUCK_SCALE  { { 500u, 5000u }, { 500u, 25000u } }
#define LDO12_SCALE { { 500u, 12500u }, { 1200u, 12500u } }
#define LDO36_SCALE { { 500u, 12500u }, { 1000u, 50000u } }

#define BUCK_R(pg, b, bank) \
	{ pg, (b), (b) + 2u, (b) + 4u, (b) + 6u, 0x02u, 0u, 0x7Fu, true, bank, BUCK_SCALE }
#define BUCK_R1(b) \
	{ ACT8760_PAGE_SYSTEM, (b), (b) + 2u, (b) + 4u, (b) + 1u, 0x08u, 0u, 0x7Fu, true, false, \
	  BUCK_SCALE }
#define BUCK_F(b, rng) \
	{ ACT8760_PAGE_SYSTEM, (b), (b) + 2u, (b) + 4u, 0u, 0u, rng, 0x7Fu, true, false, BUCK_SCALE }
#define LDO(b, rm, sc) \
	{ ACT8760_PAGE_AUX, (b), (b) + 1u, (b) + 2u, (b) + 1u, rm, 0u, 0x3Fu, false, false, sc }

/* Order matches act8760_rail_t. */
static const struct rail_loc rail_table[ACT8760_RAIL_COUNT] = {
	[ACT8760_RAIL_BUCK1] = BUCK_R(ACT8760_PAGE_SYSTEM, 0x40u, true),
	[ACT8760_RAIL_BUCK2] = BUCK_R(ACT8760_PAGE_SYSTEM, 0x60u, true),
	[ACT8760_RAIL_BUCK3] = BUCK_R1(0x80u),
	[ACT8760_RAIL_BUCK4] = BUCK_R1(0xA0u),
	[ACT8760_RAIL_BUCK5] = BUCK_F(0xC0u, 1u),
	[ACT8760_RAIL_BUCK6] = BUCK_F(0xE0u, 1u),
	[ACT8760_RAIL_BUCK7] = BUCK_R(ACT8760_PAGE_AUX, 0x00u, true),
	[ACT8760_RAIL_LDO1]  = LDO(0x20u, 0x40u, LDO12_SCALE),
	[ACT8760_RAIL_LDO2]  = LDO(0x26u, 0x40u, LDO12_SCALE),
	[ACT8760_RAIL_LDO3]  = LDO(0x46u, 0x80u, LDO36_SCALE),
	[ACT8760_RAIL_LDO4]  = LDO(0x66u, 0x80u, LDO36_SCALE),
	[ACT8760_RAIL_LDO5]  = LDO(0x40u, 0x80u, LDO36_SCALE),
	[ACT8760_RAIL_LDO6]  = LDO(0x60u, 0x80u, LDO36_SCALE),
};

/* Raw-write allow-list (ADD1 only): IRQ masks and LED current only.  0x14
 * (POK_OV[2:0] / VSYSWARN[4:0]) is deliberately NOT here even though the
 * chip lets it be written: a bad threshold there can trip an unwanted
 * PMIC shutdown, and nothing in this tree writes it -- deny by default,
 * add it back only alongside a typed, value-validated API if a real use
 * shows up.  Every other address -- including the hard-deny set MSTR 0x07
 * (MR / SLEEP / DPSLP / POWER OFF / watchdog), 0x08, 0x09, 0x0A, 0x0B/0x0C
 * (IO delays that retime PMIC_RSTOUT / V2N_BOOT_CPU_SEL / DEEPX_PWR_EN_REQ;
 * 0x0C bits1:0 are WDTIME / RETRY TIME), 0x15..0x26, 0x2C, 0x2D..0x32,
 * 0x34, every MODEx, every tile register and all of ADD2 -- is refused.
 * An allow-list, so an address nobody thought about is denied by
 * construction. */
static const uint8_t raw_write_allow[] = { 0x01u, 0x05u, 0x2Bu, 0x33u };

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

/* Read-modify-write of the @p mask bits only, then read back.  Skips the
 * bus write when the bits already hold @p bits. */
static alp_status_t
reg_update_verify(act8760_t *ctx, act8760_page_t page, uint8_t reg, uint8_t mask, uint8_t bits)
{
	uint8_t      cur = 0;
	alp_status_t s   = reg_read(ctx, page, reg, &cur);
	if (s != ALP_OK) return s;

	uint8_t want = (uint8_t)((cur & (uint8_t)~mask) | (bits & mask));
	if (want != cur) {
		s = reg_write(ctx, page, reg, want);
		if (s != ALP_OK) return s;
		s = reg_read(ctx, page, reg, &cur);
		if (s != ALP_OK) return s;
	}
	return (cur & mask) == (bits & mask) ? ALP_OK : ALP_ERR_IO;
}

static bool ready(const act8760_t *ctx)
{
	return ctx != NULL && ctx->initialised;
}

static const pmic_rail_limit_t *limit_for(const act8760_t *ctx, act8760_rail_t rail)
{
	return ctx->limits != NULL ? &ctx->limits[rail] : NULL;
}

static uint32_t decode_uv(const struct vscale *sc, uint8_t code)
{
	return (uint32_t)sc->base_mv * 1000u + (uint32_t)code * sc->step_uv;
}

static uint16_t decode_mv(const struct vscale *sc, uint8_t code)
{
	return (uint16_t)(decode_uv(sc, code) / 1000u);
}

/* Live range of @p loc; @p vset is the already-read VSET0 byte (the LDO
 * range bit shares it). */
static alp_status_t
read_range(act8760_t *ctx, const struct rail_loc *loc, uint8_t vset, uint8_t *range)
{
	if (loc->range_mask == 0u) {
		*range = loc->fixed_range;
		return ALP_OK;
	}
	uint8_t byte = vset;
	if (loc->range_reg != loc->vset_reg) {
		alp_status_t s = reg_read(ctx, loc->page, loc->range_reg, &byte);
		if (s != ALP_OK) return s;
	}
	*range = (byte & loc->range_mask) != 0u ? 1u : 0u;
	return ALP_OK;
}

/* On Buck1/2/7 the VSET0 address shows VSET2 while MSTR 0x2C BAND_SEL
 * is set; refuse to decode / program it then. */
static alp_status_t check_bank(act8760_t *ctx, const struct rail_loc *loc)
{
	if (!loc->banked) return ALP_OK;
	uint8_t      sel = 0;
	alp_status_t s   = reg_read(ctx, ACT8760_PAGE_SYSTEM, ACT8760_REG_DVS_SEL, &sel);
	if (s != ALP_OK) return s;
	return (sel & ACT8760_BAND_SEL) != 0u ? ALP_ERR_NOSUPPORT : ALP_OK;
}

/* VSET0 byte + live range of a rail, bank-checked. */
static alp_status_t
read_setpoint(act8760_t *ctx, const struct rail_loc *loc, uint8_t *vset, uint8_t *range)
{
	alp_status_t s = check_bank(ctx, loc);
	if (s != ALP_OK) return s;
	s = reg_read(ctx, loc->page, loc->vset_reg, vset);
	if (s != ALP_OK) return s;
	return read_range(ctx, loc, *vset, range);
}

static alp_status_t mode_reg_for(uint8_t gpio, uint8_t *reg)
{
	if (gpio < 1u || gpio > ACT8760_GPIO_COUNT) return ALP_ERR_INVAL;
	*reg = gpio <= 7u ? (uint8_t)(ACT8760_REG_MODE1 + gpio - 1u)
	                  : (uint8_t)(ACT8760_REG_MODE8 + gpio - 8u);
	return ALP_OK;
}

/* Fold the GPIO9..11 toggle bits of a 0x2B byte into the latch. */
static void latch_toggles_9_11(act8760_t *ctx, uint8_t r2b)
{
	if ((r2b & 0x40u) != 0u) ctx->gpio_toggles |= 1u << 8;  /* GPIO9 */
	if ((r2b & 0x08u) != 0u) ctx->gpio_toggles |= 1u << 9;  /* GPIO10 */
	if ((r2b & 0x01u) != 0u) ctx->gpio_toggles |= 1u << 10; /* GPIO11 */
}

/* Read both clear-on-read toggle registers into the context latch. */
static alp_status_t harvest_toggles(act8760_t *ctx)
{
	uint8_t      v = 0;
	alp_status_t s = reg_read(ctx, ACT8760_PAGE_SYSTEM, ACT8760_REG_GPIO_TOGGLE, &v);
	if (s != ALP_OK) return s;
	ctx->gpio_toggles = (uint16_t)(ctx->gpio_toggles | v);
	s                 = reg_read(ctx, ACT8760_PAGE_SYSTEM, ACT8760_REG_GPIO9_11, &v);
	if (s != ALP_OK) return s;
	latch_toggles_9_11(ctx, v);
	return ALP_OK;
}

alp_status_t act8760_init_at(act8760_t *ctx, alp_i2c_t *bus, uint8_t addr_page0)
{
	if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
	/* ADD2 always lives at ADD1 + 1; reject addresses that would
	 * collide with reserved I2C addresses. */
	if (addr_page0 == 0x7F) return ALP_ERR_INVAL;

	/* Also clears limits: a fresh context is fail-closed. */
	memset(ctx, 0, sizeof(*ctx));
	ctx->bus        = bus;
	ctx->addr_page0 = addr_page0;
	ctx->addr_page1 = (uint8_t)(addr_page0 + 1);

	/* Probe ADD1 by reading the GPIO level register (0x03, MSTR tile;
	 * NOT 0x00, whose VSYS latches clear on read) and ADD2 by reading
	 * register 0x00 of the Buck7 tile -- both are pure ACK checks; the
	 * returned value is ignored. */
	uint8_t tmp = 0;
	if (reg_read(ctx, ACT8760_PAGE_SYSTEM, ACT8760_REG_GPIO_STAT, &tmp) != ALP_OK) {
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
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
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
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;
	if (page != ACT8760_PAGE_SYSTEM && page != ACT8760_PAGE_AUX) return ALP_ERR_INVAL;
	return reg_read(ctx, page, reg, out);
}

alp_status_t act8760_write_reg(act8760_t *ctx, act8760_page_t page, uint8_t reg, uint8_t val)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	if (page != ACT8760_PAGE_SYSTEM && page != ACT8760_PAGE_AUX) return ALP_ERR_INVAL;
	if (ctx->limits == NULL) return ALP_ERR_NOSUPPORT;
	if (page != ACT8760_PAGE_SYSTEM) return ALP_ERR_NOSUPPORT;
	for (size_t i = 0; i < sizeof(raw_write_allow); i++) {
		if (raw_write_allow[i] == reg) return reg_write(ctx, page, reg, val);
	}
	return ALP_ERR_NOSUPPORT;
}

alp_status_t
act8760_set_limits(act8760_t *ctx, const pmic_rail_limit_t *rails, uint16_t gpio_polarity_writable)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	if ((gpio_polarity_writable >> ACT8760_GPIO_COUNT) != 0u) return ALP_ERR_INVAL;
	if (rails != NULL) {
		for (unsigned i = 0; i < ACT8760_RAIL_COUNT; i++) {
			if (rails[i].voltage_writable && rails[i].min_mv > rails[i].max_mv) {
				return ALP_ERR_INVAL;
			}
		}
	}
	ctx->limits           = rails;
	ctx->gpio_polarity_ok = rails != NULL ? gpio_polarity_writable : 0u;
	return ALP_OK;
}

alp_status_t act8760_rail_get_state(act8760_t *ctx, act8760_rail_t rail, act8760_rail_state_t *out)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	if (out == NULL || (unsigned)rail >= ACT8760_RAIL_COUNT) return ALP_ERR_INVAL;

	const struct rail_loc *loc = &rail_table[rail];
	uint8_t                st = 0, vset = 0, en = 0, range = 0;
	alp_status_t           s = reg_read(ctx, loc->page, loc->status_reg, &st);
	if (s == ALP_OK) s = reg_read(ctx, loc->page, loc->en_reg, &en);
	if (s == ALP_OK) s = reg_read(ctx, loc->page, loc->vset_reg, &vset);
	if (s == ALP_OK) s = read_range(ctx, loc, vset, &range);
	if (s != ALP_OK) return s;

	memset(out, 0, sizeof(*out));
	out->status_raw = st;
	out->pok        = (st & ACT8760_TILE_POK) != 0u;
	out->ov         = (st & ACT8760_TILE_OV) != 0u;
	out->ilim       = (st & ACT8760_TILE_ILIM) != 0u;
	out->ilim_warn  = loc->is_buck && (st & ACT8760_TILE_ILIM_WARN) != 0u;
	out->enabled    = (en & ACT8760_TILE_ON) != 0u;
	out->vset_raw   = vset & loc->vset_mask;
	out->range      = range;
	/* voltage_mv stays 0 when the VSET0 address is bank-aliased to
	 * VSET2 (BAND_SEL). */
	if (check_bank(ctx, loc) == ALP_OK) {
		out->voltage_mv = decode_mv(&loc->scale[range], out->vset_raw);
	}
	return ALP_OK;
}

alp_status_t act8760_rail_get_voltage_mv(act8760_t *ctx, act8760_rail_t rail, uint16_t *mv)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	if (mv == NULL || (unsigned)rail >= ACT8760_RAIL_COUNT) return ALP_ERR_INVAL;

	const struct rail_loc *loc  = &rail_table[rail];
	uint8_t                vset = 0, range = 0;
	alp_status_t           s = read_setpoint(ctx, loc, &vset, &range);
	if (s != ALP_OK) return s;
	*mv = decode_mv(&loc->scale[range], vset & loc->vset_mask);
	return ALP_OK;
}

alp_status_t act8760_rail_set_voltage_mv(act8760_t *ctx, act8760_rail_t rail, uint16_t mv)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	if ((unsigned)rail >= ACT8760_RAIL_COUNT) return ALP_ERR_INVAL;
	const pmic_rail_limit_t *lim = limit_for(ctx, rail);
	if (lim == NULL || !lim->voltage_writable) return ALP_ERR_NOSUPPORT;

	const struct rail_loc *loc  = &rail_table[rail];
	uint8_t                vset = 0, range = 0;
	alp_status_t           s = read_setpoint(ctx, loc, &vset, &range);
	if (s != ALP_OK) return s;

	const struct vscale *sc = &loc->scale[range];
	if (mv < sc->base_mv) return ALP_ERR_OUT_OF_RANGE;
	uint32_t code = ((uint32_t)(mv - sc->base_mv) * 1000u) / sc->step_uv; /* round down */
	if (code > loc->vset_mask) return ALP_ERR_OUT_OF_RANGE;
	/* Compare in uV: the 12.5 mV LDO grid must not truncate into the window. */
	uint32_t enc_uv = decode_uv(sc, (uint8_t)code);
	if (enc_uv < (uint32_t)lim->min_mv * 1000u || enc_uv > (uint32_t)lim->max_mv * 1000u) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	/* RMW of the VSET field only: EN_OutPD / IPD_SET / RANGE survive. */
	return reg_update_verify(ctx, loc->page, loc->vset_reg, loc->vset_mask, (uint8_t)code);
}

alp_status_t act8760_rail_set_enable(act8760_t *ctx, act8760_rail_t rail, bool enable)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	if ((unsigned)rail >= ACT8760_RAIL_COUNT) return ALP_ERR_INVAL;
	const pmic_rail_limit_t *lim = limit_for(ctx, rail);
	if (lim == NULL || !lim->enable_writable) return ALP_ERR_NOSUPPORT;
	if (!enable && lim->critical) return ALP_ERR_NOSUPPORT;

	const struct rail_loc *loc = &rail_table[rail];

	/* Enabling energizes the rail at its CURRENTLY programmed setpoint --
	 * refuse instead of blindly powering up a live VSET the guard window
	 * no longer covers.  Same rule pmic_rail_limit.h documents and
	 * da9292_set_enable() / tps628640_software_enable() already apply;
	 * lim->max_mv == 0 is the generator's "no window" sentinel (an
	 * enable-only rail such as a load switch has no VSET to check). */
	if (enable && lim->max_mv != 0u) {
		uint8_t      vset = 0, range = 0;
		alp_status_t s = read_setpoint(ctx, loc, &vset, &range);
		if (s != ALP_OK) return s;
		uint32_t enc_uv = decode_uv(&loc->scale[range], vset & loc->vset_mask);
		if (enc_uv < (uint32_t)lim->min_mv * 1000u || enc_uv > (uint32_t)lim->max_mv * 1000u) {
			return ALP_ERR_OUT_OF_RANGE;
		}
	}

	return reg_update_verify(
	    ctx, loc->page, loc->en_reg, ACT8760_TILE_ON, enable ? ACT8760_TILE_ON : 0u);
}

alp_status_t act8760_gpio_get(act8760_t *ctx, uint8_t gpio, act8760_gpio_state_t *out)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	uint8_t mode_reg = 0;
	if (out == NULL || mode_reg_for(gpio, &mode_reg) != ALP_OK) return ALP_ERR_INVAL;

	uint8_t      mode = 0, pp = 0;
	alp_status_t s = reg_read(ctx, ACT8760_PAGE_SYSTEM, mode_reg, &mode);
	if (s == ALP_OK) s = reg_read(ctx, ACT8760_PAGE_SYSTEM, ACT8760_REG_PUSH_PULL, &pp);
	if (s != ALP_OK) return s;

	bool level = false, masked = false;
	if (gpio <= 8u) {
		uint8_t bit = (uint8_t)(1u << (gpio - 1u)), stat = 0, mask = 0;
		s = reg_read(ctx, ACT8760_PAGE_SYSTEM, ACT8760_REG_GPIO_STAT, &stat);
		if (s == ALP_OK) s = reg_read(ctx, ACT8760_PAGE_SYSTEM, ACT8760_REG_GPIO_MASK, &mask);
		if (s != ALP_OK) return s;
		level  = (stat & bit) != 0u;
		masked = (mask & bit) != 0u;
	} else {
		/* 0x2B: GPIO9 b7 STAT b6 toggle b5 MASK; GPIO10 b4/b3/b2;
		 * GPIO11 b1 STAT b0 toggle, its MASK is 0x2C bit1. */
		uint8_t r2b = 0;
		s           = reg_read(ctx, ACT8760_PAGE_SYSTEM, ACT8760_REG_GPIO9_11, &r2b);
		if (s != ALP_OK) return s;
		latch_toggles_9_11(ctx, r2b);        /* the read just cleared them */
		unsigned sh = 7u - 3u * (gpio - 9u); /* STAT bit: 7, 4, 1 */
		level       = ((r2b >> sh) & 1u) != 0u;
		if (gpio < 11u) {
			masked = ((r2b >> (sh - 2u)) & 1u) != 0u;
		} else {
			uint8_t r2c = 0;
			s           = reg_read(ctx, ACT8760_PAGE_SYSTEM, ACT8760_REG_DVS_SEL, &r2c);
			if (s != ALP_OK) return s;
			masked = (r2c & ACT8760_GPIO11_MASK) != 0u;
		}
	}

	/* 0x34 bits6..0 = GPIO8,7,6,4,3,2,1; no bit for GPIO5 / GPIO9..11. */
	int pp_bit = gpio <= 4u ? (int)gpio - 1 : (gpio >= 6u && gpio <= 8u) ? (int)gpio - 2 : -1;

	out->level      = level;
	out->inverted   = (mode & ACT8760_MODE_POLARITY) != 0u;
	out->mux        = mode & ACT8760_MODE_MUX;
	out->mode_raw   = mode;
	out->push_pull  = pp_bit >= 0 && ((pp >> pp_bit) & 1u) != 0u;
	out->irq_masked = masked;
	return ALP_OK;
}

alp_status_t act8760_gpio_toggles_peek(act8760_t *ctx, uint16_t *mask)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	if (mask == NULL) return ALP_ERR_INVAL;
	alp_status_t s = harvest_toggles(ctx);
	if (s != ALP_OK) return s;
	*mask = ctx->gpio_toggles;
	return ALP_OK;
}

alp_status_t act8760_gpio_toggles_clear(act8760_t *ctx, uint16_t *mask)
{
	alp_status_t s = act8760_gpio_toggles_peek(ctx, mask);
	if (s != ALP_OK) return s;
	ctx->gpio_toggles = 0u;
	return ALP_OK;
}

alp_status_t act8760_gpio_set_polarity(act8760_t *ctx, uint8_t gpio, bool inverted)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	uint8_t mode_reg = 0;
	if (mode_reg_for(gpio, &mode_reg) != ALP_OK) return ALP_ERR_INVAL;
	if (ctx->limits == NULL) return ALP_ERR_NOSUPPORT;
	if ((ctx->gpio_polarity_ok & (1u << (gpio - 1u))) == 0u) return ALP_ERR_NOSUPPORT;

	return reg_update_verify(ctx,
	                         ACT8760_PAGE_SYSTEM,
	                         mode_reg,
	                         ACT8760_MODE_POLARITY,
	                         inverted ? ACT8760_MODE_POLARITY : 0u);
}

alp_status_t act8760_rail_get_vset(act8760_t *ctx, act8760_rail_t rail, uint8_t *vset_raw)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	if (vset_raw == NULL) return ALP_ERR_INVAL;
	if ((unsigned)rail >= ACT8760_RAIL_COUNT) return ALP_ERR_INVAL;

	const struct rail_loc *loc = &rail_table[rail];
	uint8_t                reg = 0;
	alp_status_t           s   = reg_read(ctx, loc->page, loc->vset_reg, &reg);
	if (s != ALP_OK) return s;
	*vset_raw = reg & loc->vset_mask;
	return ALP_OK;
}

void act8760_deinit(act8760_t *ctx)
{
	if (ctx == NULL) return;
	ctx->initialised = false;
	ctx->bus         = NULL;
	ctx->limits      = NULL;
}
