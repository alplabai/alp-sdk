/*
 * Copyright 2026 Alp Lab AB
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

#include "da9292_internal.h"

/* Register map (datasheet Table 12, page 34). */
#define DA9292_REG_PMC_STATUS_00   0x00u
#define DA9292_REG_PMC_STATUS_01   0x01u
#define DA9292_REG_PMC_EVENT_00    0x02u
#define DA9292_REG_PMC_EVENT_01    0x03u
#define DA9292_REG_PMC_MASK_00     0x04u
#define DA9292_REG_PMC_MASK_01     0x05u
#define DA9292_REG_PMC_CTRL_00     0x06u
#define DA9292_REG_PMC_CTRL_01     0x07u /* CHx_EN / CHx_VSEL / VSTEP / DIS_PD */
#define DA9292_REG_PMC_CTRL_02     0x08u
#define DA9292_REG_PMC_CTRL_03     0x09u
#define DA9292_REG_PMC_VOUT_CH1_00 0x0Au /* CH1 VSEL=0 setpoint (LO range) */
#define DA9292_REG_PMC_VOUT_CH1_01 0x0Bu /* CH1 VSEL=1 retention */
#define DA9292_REG_PMC_VOUT_CH2_00 0x0Cu /* CH2 VSEL=0 */
#define DA9292_REG_PMC_VOUT_CH2_01 0x0Du /* CH2 VSEL=1 */
#define DA9292_REG_PMC_CFG_00      0x0Eu /* EN2_EN / VSEL2_EN pin functions (OTP 0xFF) */
#define DA9292_REG_PMC_DEV_ID      0x19u
#define DA9292_REG_PMC_REV_ID      0x1Au
#define DA9292_REG_PMC_CFG_REV     0x1Bu

/* PMC_CTRL_01 bit positions (datasheet Table 21, page 40-41). */
#define DA9292_CTRL01_CH2_VSTEP  (1u << 7)
#define DA9292_CTRL01_CH1_VSTEP  (1u << 6)
#define DA9292_CTRL01_CH2_DIS_PD (1u << 5)
#define DA9292_CTRL01_CH1_DIS_PD (1u << 4)
#define DA9292_CTRL01_CH2_VSEL   (1u << 3)
#define DA9292_CTRL01_CH1_VSEL   (1u << 2)
#define DA9292_CTRL01_CH2_EN     (1u << 1)
#define DA9292_CTRL01_CH1_EN     (1u << 0)

/* PMC_STATUS_00 bit layout -- VERIFIED against DA9292 Datasheet
 * Rev 2.2 (R16DS0518EJ0220), Table 14 (p.36-37) on 2026-06-06:
 * bits[7:0] = S_CH2_OC, S_CH1_OC, S_CH2_OV, S_CH1_OV,
 *             S_CH2_UV, S_CH1_UV, S_CH2_PG, S_CH1_PG.
 * (Same ordering as PMC_MASK_00, Table 18 -- the historical
 * mirror-the-mask assumption checked out.) */
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
#define DA9292_EVENT00_CH2_OC    (1u << 7)
#define DA9292_EVENT00_CH1_OC    (1u << 6)
#define DA9292_EVENT00_CH2_OV    (1u << 5)
#define DA9292_EVENT00_CH1_OV    (1u << 4)
#define DA9292_EVENT00_CH2_UV    (1u << 3)
#define DA9292_EVENT00_CH1_UV    (1u << 2)
#define DA9292_EVENT00_CH2_PG    (1u << 1)
#define DA9292_EVENT00_CH1_PG    (1u << 0)
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

/* VOUT code encoding (datasheet Table 24): VOUT = code * 5 mV at
 * VSTEP=0 (0x3C = 0.300 V .. 0xFF = 1.275 V); VSTEP=1 doubles it (10 mV
 * step), with the output capped at 1.90 V (code 0xBE).  Codes 0x00..0x3B
 * are reserved.  0x96 = 0.750 V at VSTEP=0. */
#define DA9292_VOUT_CODE_MIN      0x3Cu
#define DA9292_VOUT_STEP_MV       5u
#define DA9292_VOUT_VSTEP1_MAX_MV 1900u

/* Raw-writable window of da9292_write_reg(): EVENT_00/01 (W1C) + MASK_00/01. */
#define DA9292_RAW_WRITE_FIRST DA9292_REG_PMC_EVENT_00
#define DA9292_RAW_WRITE_LAST  DA9292_REG_PMC_MASK_01

static bool ch_valid(da9292_channel_t ch)
{
	return ch == DA9292_CH1 || ch == DA9292_CH2;
}

static uint8_t vout_lo_reg(da9292_channel_t ch)
{
	return ch == DA9292_CH1 ? DA9292_REG_PMC_VOUT_CH1_00 : DA9292_REG_PMC_VOUT_CH2_00;
}

static uint8_t vout_hi_reg(da9292_channel_t ch)
{
	return ch == DA9292_CH1 ? DA9292_REG_PMC_VOUT_CH1_01 : DA9292_REG_PMC_VOUT_CH2_01;
}

static uint8_t en_bit_for(da9292_channel_t ch)
{
	return ch == DA9292_CH1 ? DA9292_CTRL01_CH1_EN : DA9292_CTRL01_CH2_EN;
}

static uint8_t vstep_bit_for(da9292_channel_t ch)
{
	return ch == DA9292_CH1 ? DA9292_CTRL01_CH1_VSTEP : DA9292_CTRL01_CH2_VSTEP;
}

static uint8_t vsel_bit_for(da9292_channel_t ch)
{
	return ch == DA9292_CH1 ? DA9292_CTRL01_CH1_VSEL : DA9292_CTRL01_CH2_VSEL;
}

/* Decode a VOUT code through a VSTEP setting; 0 = reserved code. */
static uint16_t vout_decode(uint8_t code, bool vstep)
{
	if (code < DA9292_VOUT_CODE_MIN) return 0u;
	uint32_t mv = (uint32_t)code * DA9292_VOUT_STEP_MV;
	if (vstep) {
		mv *= 2u;
		if (mv > DA9292_VOUT_VSTEP1_MAX_MV) mv = DA9292_VOUT_VSTEP1_MAX_MV;
	}
	return (uint16_t)mv;
}

/* Encode mV for a VSTEP setting (rounded down to the grid).  Returns
 * false when no valid code produces it (reserved code, beyond 0xFF, or
 * past the VSTEP=1 1.90 V cap). */
static bool vout_encode(uint16_t mv, bool vstep, uint8_t *code)
{
	const uint32_t step = vstep ? 2u * DA9292_VOUT_STEP_MV : DA9292_VOUT_STEP_MV;
	const uint32_t c    = mv / step;
	if (c < DA9292_VOUT_CODE_MIN || c > 0xFFu) return false;
	if (vstep && c * step > DA9292_VOUT_VSTEP1_MAX_MV) return false;
	*code = (uint8_t)c;
	return true;
}

static bool in_window(const pmic_rail_limit_t *l, uint16_t mv)
{
	return mv != 0u && mv >= l->min_mv && mv <= l->max_mv;
}

static alp_status_t reg_read(da9292_t *ctx, uint8_t reg, uint8_t *val)
{
	return alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, val, 1);
}

static alp_status_t reg_write(da9292_t *ctx, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	return alp_i2c_write(ctx->bus, ctx->addr, buf, sizeof(buf));
}

bool da9292_poll_budget_step(uint32_t *remaining_us, uint32_t poll_us)
{
	if (*remaining_us < poll_us) return false;
	*remaining_us -= poll_us;
	return true;
}

alp_status_t da9292_init(da9292_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit)
{
	if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (addr_7bit > 0x7F) return ALP_ERR_INVAL;

	/* Also clears ctx->limits: a fresh context is fail-closed. */
	memset(ctx, 0, sizeof(*ctx));
	ctx->bus  = bus;
	ctx->addr = addr_7bit;

	/* Probe via PMC_DEV_ID.  Only "ACKs and non-blank" here -- the exact
	 * OTP variant is checked where it matters (da9292_ch2_sequence()). */
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

alp_status_t da9292_set_limits(da9292_t *ctx, const pmic_rail_limit_t *ch_limits)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (ch_limits != NULL) {
		for (unsigned i = 0; i < DA9292_CH_COUNT; i++) {
			if (ch_limits[i].voltage_writable && ch_limits[i].min_mv > ch_limits[i].max_mv)
				return ALP_ERR_INVAL;
		}
	}
	ctx->limits = ch_limits;
	return ALP_OK;
}

alp_status_t da9292_get_identity(da9292_t *ctx, da9292_identity_t *out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;

	alp_status_t s = reg_read(ctx, DA9292_REG_PMC_DEV_ID, &out->dev_id);
	if (s == ALP_OK) s = reg_read(ctx, DA9292_REG_PMC_REV_ID, &out->rev_id);
	if (s == ALP_OK) s = reg_read(ctx, DA9292_REG_PMC_CFG_REV, &out->cfg_rev);
	return s;
}

alp_status_t
da9292_get_channel_state(da9292_t *ctx, da9292_channel_t ch, da9292_channel_state_t *out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL || !ch_valid(ch)) return ALP_ERR_INVAL;

	uint8_t      ctrl = 0, lo = 0, hi = 0, s00 = 0;
	alp_status_t s = reg_read(ctx, DA9292_REG_PMC_CTRL_01, &ctrl);
	if (s == ALP_OK) s = reg_read(ctx, vout_lo_reg(ch), &lo);
	if (s == ALP_OK) s = reg_read(ctx, vout_hi_reg(ch), &hi);
	if (s == ALP_OK) s = reg_read(ctx, DA9292_REG_PMC_STATUS_00, &s00);
	if (s != ALP_OK) return s;

	const bool c1   = ch == DA9292_CH1;
	out->enabled    = (ctrl & en_bit_for(ch)) != 0;
	out->vstep      = (ctrl & vstep_bit_for(ch)) != 0;
	out->vsel_hi    = (ctrl & vsel_bit_for(ch)) != 0;
	out->vsel_lo_mv = vout_decode(lo, out->vstep);
	out->vsel_hi_mv = vout_decode(hi, out->vstep);
	out->pg         = (s00 & (c1 ? DA9292_STATUS00_CH1_PG : DA9292_STATUS00_CH2_PG)) != 0;
	out->uv         = (s00 & (c1 ? DA9292_STATUS00_CH1_UV : DA9292_STATUS00_CH2_UV)) != 0;
	out->ov         = (s00 & (c1 ? DA9292_STATUS00_CH1_OV : DA9292_STATUS00_CH2_OV)) != 0;
	out->oc         = (s00 & (c1 ? DA9292_STATUS00_CH1_OC : DA9292_STATUS00_CH2_OC)) != 0;
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
	/* Fail-closed; CTRL_01 / VOUT / everything else is typed-API-only. */
	if (ctx->limits == NULL) return ALP_ERR_NOSUPPORT;
	if (reg < DA9292_RAW_WRITE_FIRST || reg > DA9292_RAW_WRITE_LAST) return ALP_ERR_NOSUPPORT;
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

	out->raw_00    = s00;
	out->raw_01    = s01;
	out->ch1_oc    = (s00 & DA9292_STATUS00_CH1_OC) != 0;
	out->ch2_oc    = (s00 & DA9292_STATUS00_CH2_OC) != 0;
	out->ch1_ov    = (s00 & DA9292_STATUS00_CH1_OV) != 0;
	out->ch2_ov    = (s00 & DA9292_STATUS00_CH2_OV) != 0;
	out->ch1_uv    = (s00 & DA9292_STATUS00_CH1_UV) != 0;
	out->ch2_uv    = (s00 & DA9292_STATUS00_CH2_UV) != 0;
	out->ch1_pg    = (s00 & DA9292_STATUS00_CH1_PG) != 0;
	out->ch2_pg    = (s00 & DA9292_STATUS00_CH2_PG) != 0;
	out->temp_warn = (s01 & DA9292_STATUS01_TEMP_WARN) != 0;
	out->temp_crit = (s01 & DA9292_STATUS01_TEMP_CRIT) != 0;
	out->vin_uvlo  = (s01 & DA9292_STATUS01_VIN_UVLO) != 0;
	return ALP_OK;
}

static void events_decode(uint8_t e00, uint8_t e01, da9292_events_t *out)
{
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
	out->raw_00      = e00;
	out->raw_01      = e01;
}

alp_status_t da9292_peek_events(da9292_t *ctx, da9292_events_t *out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;

	uint8_t      e00 = 0, e01 = 0;
	alp_status_t s = reg_read(ctx, DA9292_REG_PMC_EVENT_00, &e00);
	if (s == ALP_OK) s = reg_read(ctx, DA9292_REG_PMC_EVENT_01, &e01);
	if (s != ALP_OK) return s;
	events_decode(e00, e01, out);
	return ALP_OK;
}

alp_status_t da9292_read_and_clear_events(da9292_t *ctx, da9292_events_t *out)
{
	alp_status_t s = da9292_peek_events(ctx, out);
	if (s != ALP_OK) return s;
	/* The W1C is a control write: no table => report, don't clear. */
	if (ctx->limits == NULL) return ALP_ERR_NOSUPPORT;

	/* Write-1-to-clear exactly the bits read -- an event latching between
	 * the read and the write survives for the next call. */
	if (out->raw_00 != 0) s = reg_write(ctx, DA9292_REG_PMC_EVENT_00, out->raw_00);
	if (s == ALP_OK && out->raw_01 != 0) s = reg_write(ctx, DA9292_REG_PMC_EVENT_01, out->raw_01);
	return s;
}

alp_status_t da9292_get_fault_pins(alp_gpio_t *int_n, alp_gpio_t *tw_n, uint8_t *flags)
{
	if (flags == NULL) return ALP_ERR_INVAL;

	/* Both fault outputs are open-drain ACTIVE-LOW (INT_N / TW_N):
     * asserted = pin reads low.  Packing mirrors the GD32 bridge's
     * DA9292_STATUS_FORWARD reply byte (bit0 = INT, bit1 = TW) so the
     * two paths stay drop-in compatible.  A NULL pin reports its bit
     * deasserted -- the caller's board may wire only one of the two. */
	uint8_t packed = 0u;
	bool    level;

	if (int_n != NULL) {
		alp_status_t s = alp_gpio_read(int_n, &level);
		if (s != ALP_OK) return s;
		if (!level) packed |= 0x01u;
	}
	if (tw_n != NULL) {
		alp_status_t s = alp_gpio_read(tw_n, &level);
		if (s != ALP_OK) return s;
		if (!level) packed |= 0x02u;
	}
	*flags = packed;
	return ALP_OK;
}

/* The guard entry for @p ch, or NULL when no table is installed. */
static const pmic_rail_limit_t *limit_for(const da9292_t *ctx, da9292_channel_t ch)
{
	return ctx->limits != NULL ? &ctx->limits[ch] : NULL;
}

alp_status_t da9292_set_enable(da9292_t *ctx, da9292_channel_t ch, bool enable)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (!ch_valid(ch)) return ALP_ERR_INVAL;

	const pmic_rail_limit_t *l = limit_for(ctx, ch);
	if (l == NULL || !l->enable_writable) return ALP_ERR_NOSUPPORT;
	if (!enable && l->critical) return ALP_ERR_NOSUPPORT;

	uint8_t      ctrl = 0;
	alp_status_t s    = reg_read(ctx, DA9292_REG_PMC_CTRL_01, &ctrl);
	if (s != ALP_OK) return s;

	const uint8_t en = en_bit_for(ch);
	if (enable == ((ctrl & en) != 0)) return ALP_OK; /* already there: no write */

	/* An enable-only entry has no window: CH2 would come up at the OTP
	 * 1.80 V.  The DA9292 is never enabled without a window. */
	if (enable && l->max_mv == 0u) return ALP_ERR_NOSUPPORT;
	if (enable) {
		/* Both setpoints must be in-window: the VSELx PIN may select
		 * either one regardless of the CHx_VSEL register bit. */
		const bool vstep = (ctrl & vstep_bit_for(ch)) != 0;
		uint8_t    lo = 0, hi = 0;
		s = reg_read(ctx, vout_lo_reg(ch), &lo);
		if (s == ALP_OK) s = reg_read(ctx, vout_hi_reg(ch), &hi);
		if (s != ALP_OK) return s;
		if (!in_window(l, vout_decode(lo, vstep)) || !in_window(l, vout_decode(hi, vstep)))
			return ALP_ERR_OUT_OF_RANGE;
	}
	return reg_write(ctx, DA9292_REG_PMC_CTRL_01, (uint8_t)(ctrl ^ en));
}

alp_status_t da9292_set_voltage_mv(da9292_t *ctx, da9292_channel_t ch, uint16_t mv)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (!ch_valid(ch)) return ALP_ERR_INVAL;

	const pmic_rail_limit_t *l = limit_for(ctx, ch);
	if (l == NULL || !l->voltage_writable) return ALP_ERR_NOSUPPORT;

	uint8_t      ctrl = 0;
	alp_status_t s    = reg_read(ctx, DA9292_REG_PMC_CTRL_01, &ctrl);
	if (s != ALP_OK) return s;

	/* Encode for the LIVE VSTEP and guard the ENCODED voltage, so a
	 * VSTEP=1 channel can never land at double the intended setpoint. */
	const bool vstep = (ctrl & vstep_bit_for(ch)) != 0;
	uint8_t    code  = 0;
	if (!vout_encode(mv, vstep, &code) || !in_window(l, vout_decode(code, vstep)))
		return ALP_ERR_OUT_OF_RANGE;

	/* VSELx pin routing is undocumented on V2N: program both setpoints. */
	uint8_t lo = 0, hi = 0;
	s = reg_write(ctx, vout_lo_reg(ch), code);
	if (s == ALP_OK) s = reg_write(ctx, vout_hi_reg(ch), code);
	if (s == ALP_OK) s = reg_read(ctx, vout_lo_reg(ch), &lo);
	if (s == ALP_OK) s = reg_read(ctx, vout_hi_reg(ch), &hi);
	if (s != ALP_OK) return s;
	return (lo == code && hi == code) ? ALP_OK : ALP_ERR_IO;
}

alp_status_t da9292_get_voltage_mv(da9292_t *ctx, da9292_channel_t ch, uint16_t *mv)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (mv == NULL || !ch_valid(ch)) return ALP_ERR_INVAL;

	uint8_t      ctrl = 0, code = 0;
	alp_status_t s = reg_read(ctx, DA9292_REG_PMC_CTRL_01, &ctrl);
	if (s != ALP_OK) return s;
	const bool hi = (ctrl & vsel_bit_for(ch)) != 0;
	s             = reg_read(ctx, hi ? vout_hi_reg(ch) : vout_lo_reg(ch), &code);
	if (s != ALP_OK) return s;

	const uint16_t v = vout_decode(code, (ctrl & vstep_bit_for(ch)) != 0);
	if (v == 0u) return ALP_ERR_IO; /* reserved code */
	*mv = v;
	return ALP_OK;
}

/* ---- da9292_ch2_sequence() ------------------------------------------ *
 * Follows the same register sequence as meta-alp-sdk/recipes-bsp/u-boot/
 * u-boot/0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch
 * (alp_deepx_rail_bringup); the numbered comments match the header's
 * list.  Two deliberate differences from the patch: (a) OS hooks -- GPIOs
 * are caller-opened alp_gpio_t, delays go through cfg->delay (1 ms
 * slices, like the patch's mdelay(1) polls); (b) step 6 clears CH2_EN and
 * CH2_VSTEP as two separate read-modify-writes, read back after each,
 * where the patch clears both bits in one combined write -- a partial
 * failure here can never leave VSTEP cleared with EN still set. */

#define DA9292_CH2_FAULT_MASK \
	(DA9292_STATUS00_CH2_UV | DA9292_STATUS00_CH2_OV | DA9292_STATUS00_CH2_OC)

/* alp_deepx_rail_safe_off(): DEEPX_CORE_0P75_EN low, M1_RESET asserted. */
static alp_status_t seq_fail(const struct da9292_ch2_seq_cfg *cfg, alp_status_t rc)
{
	(void)alp_gpio_write(cfg->core_en, false);
	if (cfg->m1_reset != NULL) (void)alp_gpio_write(cfg->m1_reset, false);
	return rc;
}

static void delay_ms(const struct da9292_ch2_seq_cfg *cfg, uint32_t ms)
{
	while (ms-- > 0u)
		cfg->delay(cfg->delay_user, 1000u);
}

alp_status_t da9292_ch2_sequence(da9292_t                        *ctx,
                                 const struct da9292_ch2_seq_cfg *cfg,
                                 struct da9292_ch2_seq_result    *res)
{
	struct da9292_ch2_seq_result local;
	if (res == NULL) res = &local;
	memset(res, 0, sizeof(*res));
	res->step = DA9292_SEQ_ERR_ARGS;

	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (cfg == NULL || cfg->pwr_en_req == NULL || cfg->core_en == NULL || cfg->delay == NULL)
		return ALP_ERR_INVAL;

	/* 1. Guard: table installed, CH2 voltage+enable writable, target
	 *    (encoded at VSTEP=0, which step 6 forces) inside the window. */
	const pmic_rail_limit_t *l = limit_for(ctx, DA9292_CH2);
	if (l == NULL || !l->voltage_writable || !l->enable_writable) return ALP_ERR_NOSUPPORT;
	uint8_t code = 0;
	if (!vout_encode(cfg->target_mv, false, &code) || !in_window(l, vout_decode(code, false)))
		return ALP_ERR_OUT_OF_RANGE;

	/* 2. Identity -- abort on anything but the exact OTP variant. */
	res->step      = DA9292_SEQ_ERR_IDENTITY;
	alp_status_t s = reg_read(ctx, DA9292_REG_PMC_DEV_ID, &res->identity.dev_id);
	(void)reg_read(ctx, DA9292_REG_PMC_REV_ID, &res->identity.rev_id);
	(void)reg_read(ctx, DA9292_REG_PMC_CFG_REV, &res->identity.cfg_rev);
	if (s != ALP_OK) return seq_fail(cfg, ALP_ERR_IO);
	if (res->identity.dev_id != cfg->expected_dev_id) return seq_fail(cfg, ALP_ERR_NOSUPPORT);

	/* 3. No thermal / UVLO status latched on entry. */
	res->step = DA9292_SEQ_ERR_STATUS01;
	if (reg_read(ctx, DA9292_REG_PMC_STATUS_01, &res->status_01) != ALP_OK)
		return seq_fail(cfg, ALP_ERR_IO);
	if (res->status_01 != 0x00u) return seq_fail(cfg, ALP_ERR_BUSY);

	/* 4. Pre-program snapshot. */
	res->step       = DA9292_SEQ_ERR_PREREAD;
	uint8_t ch1_pre = 0, ch1_post = 0;
	s = reg_read(ctx, DA9292_REG_PMC_CTRL_01, &res->ctrl_01);
	if (s == ALP_OK) s = reg_read(ctx, DA9292_REG_PMC_VOUT_CH2_00, &res->vout_ch2_00);
	if (s == ALP_OK) s = reg_read(ctx, DA9292_REG_PMC_VOUT_CH2_01, &res->vout_ch2_01);
	if (s == ALP_OK) s = reg_read(ctx, DA9292_REG_PMC_VOUT_CH1_00, &ch1_pre);
	if (s == ALP_OK) s = reg_read(ctx, DA9292_REG_PMC_CFG_00, &res->pmc_cfg_00);
	if (s != ALP_OK) return seq_fail(cfg, ALP_ERR_IO);

	/* 5. Warm-reboot idempotent path: already VSTEP=0 at target means
	 *    CH2_EN may be live with DEEPX drawing off it -- skip 6-7, never
	 *    clear it. */
	res->already_programmed = (res->ctrl_01 & DA9292_CTRL01_CH2_VSTEP) == 0 &&
	                          res->vout_ch2_00 == code && res->vout_ch2_01 == code;
	/* Warm reboot with CH2 already live: a missing request (step 10) must
	 * not drop the running rail or its enable pin. */
	const bool warm_live = res->already_programmed && (res->ctrl_01 & DA9292_CTRL01_CH2_EN) != 0;

	if (!res->already_programmed) {
		/* 6. Clear CH2_EN FIRST, then VSTEP (datasheet: VSTEP may only
		 *    change with the channel off), each read-modify-write and
		 *    read back.  A critical CH2 is never switched off. */
		res->step = DA9292_SEQ_ERR_CLEAR_VSTEP;
		if ((res->ctrl_01 & DA9292_CTRL01_CH2_EN) != 0 && l->critical)
			return seq_fail(cfg, ALP_ERR_NOSUPPORT);
		static const uint8_t clr[2] = { DA9292_CTRL01_CH2_EN, DA9292_CTRL01_CH2_VSTEP };
		for (unsigned i = 0; i < 2u; i++) {
			if ((res->ctrl_01 & clr[i]) == 0) continue;
			const uint8_t want = (uint8_t)(res->ctrl_01 & ~clr[i]);
			s                  = reg_write(ctx, DA9292_REG_PMC_CTRL_01, want);
			if (s == ALP_OK) s = reg_read(ctx, DA9292_REG_PMC_CTRL_01, &res->ctrl_01);
			if (s != ALP_OK || res->ctrl_01 != want) return seq_fail(cfg, ALP_ERR_IO);
		}

		/* 7. Program both CH2 setpoints (VSEL2 routing unknown), read back. */
		res->step = DA9292_SEQ_ERR_PROGRAM_VOUT;
		s         = reg_write(ctx, DA9292_REG_PMC_VOUT_CH2_00, code);
		if (s == ALP_OK) s = reg_write(ctx, DA9292_REG_PMC_VOUT_CH2_01, code);
		if (s == ALP_OK) s = reg_read(ctx, DA9292_REG_PMC_VOUT_CH2_00, &res->vout_ch2_00);
		if (s == ALP_OK) s = reg_read(ctx, DA9292_REG_PMC_VOUT_CH2_01, &res->vout_ch2_01);
		if (s != ALP_OK || res->vout_ch2_00 != code || res->vout_ch2_01 != code)
			return seq_fail(cfg, ALP_ERR_IO);
	}

	/* 8. (both paths) CH1 untouched and still enabled. */
	res->step = DA9292_SEQ_ERR_CH1_DISTURBED;
	s         = reg_read(ctx, DA9292_REG_PMC_VOUT_CH1_00, &ch1_post);
	if (s == ALP_OK) s = reg_read(ctx, DA9292_REG_PMC_CTRL_01, &res->ctrl_01);
	if (s != ALP_OK || ch1_post != ch1_pre || (res->ctrl_01 & DA9292_CTRL01_CH1_EN) == 0)
		return seq_fail(cfg, ALP_ERR_IO);

	/* 9. Clear latched events so a stale fault can't false-trip step 12. */
	res->step = DA9292_SEQ_ERR_EVENTS;
	s         = reg_read(ctx, DA9292_REG_PMC_EVENT_00, &res->event_00);
	if (s == ALP_OK) s = reg_read(ctx, DA9292_REG_PMC_EVENT_01, &res->event_01);
	if (s == ALP_OK && res->event_00 != 0)
		s = reg_write(ctx, DA9292_REG_PMC_EVENT_00, res->event_00);
	if (s == ALP_OK && res->event_01 != 0)
		s = reg_write(ctx, DA9292_REG_PMC_EVENT_01, res->event_01);
	if (s != ALP_OK) return seq_fail(cfg, ALP_ERR_IO);

	/* 10. Enable only on request: DEEPX_PWR_EN_REQ high within the budget.
	 *     Same bounded count-down as the PG poll (#757). */
	res->step     = DA9292_SEQ_ERR_NO_REQUEST;
	uint32_t left = cfg->req_timeout_ms;
	for (;;) {
		bool level = false;
		if (alp_gpio_read(cfg->pwr_en_req, &level) != ALP_OK) return seq_fail(cfg, ALP_ERR_IO);
		if (level) break;
		if (!da9292_poll_budget_step(&left, 1u)) {
			/* Live warm rail: leave CH2_EN and the pins exactly as found. */
			if (warm_live) return ALP_ERR_NOT_READY;
			return seq_fail(cfg, ALP_ERR_NOT_READY);
		}
		cfg->delay(cfg->delay_user, 1000u);
	}

	/* 11. Fresh CTRL_01; VSTEP=1 here is the exact over-voltage this
	 *     sequence exists to prevent -- abort (clearing a set CH2_EN). */
	res->step = DA9292_SEQ_ERR_VSTEP_AT_ENABLE;
	if (reg_read(ctx, DA9292_REG_PMC_CTRL_01, &res->ctrl_01) != ALP_OK)
		return seq_fail(cfg, ALP_ERR_IO);
	if ((res->ctrl_01 & DA9292_CTRL01_CH2_VSTEP) != 0) {
		(void)seq_fail(cfg, ALP_ERR_IO);
		if ((res->ctrl_01 & DA9292_CTRL01_CH2_EN) != 0)
			(void)reg_write(
			    ctx, DA9292_REG_PMC_CTRL_01, (uint8_t)(res->ctrl_01 & ~DA9292_CTRL01_CH2_EN));
		return ALP_ERR_IO;
	}
	res->step             = DA9292_SEQ_ERR_ENABLE;
	const uint8_t ctrl_en = (uint8_t)(res->ctrl_01 | DA9292_CTRL01_CH2_EN);
	if (ctrl_en != res->ctrl_01 && reg_write(ctx, DA9292_REG_PMC_CTRL_01, ctrl_en) != ALP_OK)
		return seq_fail(cfg, ALP_ERR_IO);
	res->ctrl_01 = ctrl_en;

	/* 12. CH2_PG with no UV/OV/OC, then no latched CH2 OV/OC event.
	 *     Stricter than the patch: an unreadable EVENT_00 fails too. */
	res->step  = DA9292_SEQ_ERR_PG_TIMEOUT;
	bool pg_ok = false;
	left       = cfg->pg_timeout_ms;
	for (;;) {
		if (reg_read(ctx, DA9292_REG_PMC_STATUS_00, &res->status_00) == ALP_OK &&
		    (res->status_00 & DA9292_STATUS00_CH2_PG) != 0 &&
		    (res->status_00 & DA9292_CH2_FAULT_MASK) == 0) {
			pg_ok = true;
			break;
		}
		if (!da9292_poll_budget_step(&left, 1u)) break;
		cfg->delay(cfg->delay_user, 1000u);
	}
	if (!pg_ok || reg_read(ctx, DA9292_REG_PMC_EVENT_00, &res->event_00) != ALP_OK ||
	    (res->event_00 & (DA9292_EVENT00_CH2_OV | DA9292_EVENT00_CH2_OC)) != 0) {
		/* Never leave CH2_EN set on a failed enable. */
		(void)reg_write(ctx, DA9292_REG_PMC_CTRL_01, (uint8_t)(ctrl_en & ~DA9292_CTRL01_CH2_EN));
		return seq_fail(cfg, ALP_ERR_TIMEOUT);
	}

	/* 13. DEEPX_CORE_0P75_EN high, settle, PG must hold with no UV / OV /
	 *     OC -- status AND latched event.  The AROVx OTP sets "OV has no
	 *     effect on PG" (PMC_CFG_07 = 0x04), so PG alone misses an OV. */
	res->step = DA9292_SEQ_ERR_PG_DROPPED;
	s         = alp_gpio_write(cfg->core_en, true);
	delay_ms(cfg, cfg->pg_settle_ms);
	if (s == ALP_OK) s = reg_read(ctx, DA9292_REG_PMC_STATUS_00, &res->status_00);
	if (s == ALP_OK) s = reg_read(ctx, DA9292_REG_PMC_EVENT_00, &res->event_00);
	if (s != ALP_OK || (res->status_00 & DA9292_STATUS00_CH2_PG) == 0 ||
	    (res->status_00 & DA9292_CH2_FAULT_MASK) != 0 ||
	    (res->event_00 & (DA9292_EVENT00_CH2_OV | DA9292_EVENT00_CH2_OC)) != 0) {
		/* Pins low FIRST (safe whatever the EN2 pin semantics), then
		 * clear CH2_EN, then safe-off again as the last word. */
		(void)seq_fail(cfg, ALP_ERR_IO);
		uint8_t ctrl = ctrl_en;
		(void)reg_read(ctx, DA9292_REG_PMC_CTRL_01, &ctrl);
		(void)reg_write(ctx, DA9292_REG_PMC_CTRL_01, (uint8_t)(ctrl & ~DA9292_CTRL01_CH2_EN));
		return seq_fail(cfg, ALP_ERR_IO);
	}

	res->rail_up = true;
	res->step    = DA9292_SEQ_OK;
	return ALP_OK;
}

void da9292_deinit(da9292_t *ctx)
{
	if (ctx == NULL) return;
	ctx->initialised = false;
	ctx->bus         = NULL;
	ctx->limits      = NULL;
}
