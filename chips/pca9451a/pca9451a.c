/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * NXP PCA9451A PMIC driver.  See <alp/chips/pca9451a.h> for the
 * public API and full register-map / voltage-range provenance.
 * Register addresses, bitmasks, and voltage-range formulas are taken
 * from the upstream Linux kernel's NXP-authored PCA9450-family driver
 * (include/linux/regulator/pca9450.h + drivers/regulator/pca9450-regulator.c,
 * Copyright 2020 NXP, GPL-2.0-or-later), which explicitly lists
 * PCA9451A as a member of that family.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/pca9451a.h"

/* Which linear-range formula a rail's vsel code decodes with.
 * Source: drivers/regulator/pca9450-regulator.c linear_range tables
 * (pca9450_dvs_buck_volts / pca9450_buck_volts / pca9450_ldo1_volts /
 * pca9450_ldo2_volts / pca9450_ldo34_volts / pca9450_ldo5_volts). */
typedef enum {
	RAIL_KIND_DVS_BUCK, /* BUCK1-3: 0.60..2.1875V, 12.5mV step, code 0x00-0x7F. */
	RAIL_KIND_STD_BUCK, /* BUCK4-6: 0.60..3.40V, 25mV step 0x00-0x70; 0x71-0x7F -> 3.40V. */
	RAIL_KIND_LDO1,     /* 1.6-1.9V (0x00-0x03) or 3.0-3.3V (0x04-0x07), 100mV step. */
	RAIL_KIND_LDO2,     /* 0.8-1.15V, 50mV step, 0x00-0x07. */
	RAIL_KIND_LDO34,    /* 0.8-3.3V, 100mV step 0x00-0x19; 0x1A-0x1F -> 3.3V. */
	RAIL_KIND_LDO5,     /* 1.8-3.3V, 100mV step, 0x00-0x0F. */
} rail_kind_t;

struct rail_loc {
	uint8_t     vsel_reg;
	uint8_t     vsel_mask;
	uint8_t     en_reg;
	uint8_t     en_mask;
	rail_kind_t kind;
};

/* Order matches pca9451a_rail_t.  Register/mask values verified
 * against pca9450a_regulators[] in the upstream driver (see the file
 * header comment above for the exact source). */
static const struct rail_loc rail_table[PCA9451A_RAIL_COUNT] = {
	[PCA9451A_RAIL_BUCK1] = { PCA9451A_REG_BUCK1OUT_DVS0,
	                          0x7F,
	                          PCA9451A_REG_BUCK1CTRL,
	                          0x03,
	                          RAIL_KIND_DVS_BUCK },
	[PCA9451A_RAIL_BUCK2] = { PCA9451A_REG_BUCK2OUT_DVS0,
	                          0x7F,
	                          PCA9451A_REG_BUCK2CTRL,
	                          0x03,
	                          RAIL_KIND_DVS_BUCK },
	[PCA9451A_RAIL_BUCK3] = { PCA9451A_REG_BUCK3OUT_DVS0,
	                          0x7F,
	                          PCA9451A_REG_BUCK3CTRL,
	                          0x03,
	                          RAIL_KIND_DVS_BUCK },
	[PCA9451A_RAIL_BUCK4] = { PCA9451A_REG_BUCK4OUT,
	                          0x7F,
	                          PCA9451A_REG_BUCK4CTRL,
	                          0x03,
	                          RAIL_KIND_STD_BUCK },
	[PCA9451A_RAIL_BUCK5] = { PCA9451A_REG_BUCK5OUT,
	                          0x7F,
	                          PCA9451A_REG_BUCK5CTRL,
	                          0x03,
	                          RAIL_KIND_STD_BUCK },
	[PCA9451A_RAIL_BUCK6] = { PCA9451A_REG_BUCK6OUT,
	                          0x7F,
	                          PCA9451A_REG_BUCK6CTRL,
	                          0x03,
	                          RAIL_KIND_STD_BUCK },
	[PCA9451A_RAIL_LDO1]  = { PCA9451A_REG_LDO1CTRL,
	                          0x07,
	                          PCA9451A_REG_LDO1CTRL,
	                          0xC0,
	                          RAIL_KIND_LDO1 },
	[PCA9451A_RAIL_LDO2]  = { PCA9451A_REG_LDO2CTRL,
	                          0x07,
	                          PCA9451A_REG_LDO2CTRL,
	                          0xC0,
	                          RAIL_KIND_LDO2 },
	[PCA9451A_RAIL_LDO3]  = { PCA9451A_REG_LDO3CTRL,
	                          0x1F,
	                          PCA9451A_REG_LDO3CTRL,
	                          0xC0,
	                          RAIL_KIND_LDO34 },
	[PCA9451A_RAIL_LDO4]  = { PCA9451A_REG_LDO4CTRL,
	                          0x1F,
	                          PCA9451A_REG_LDO4CTRL,
	                          0xC0,
	                          RAIL_KIND_LDO34 },
	/* LDO5CTRL_H: the register live when the board's SD_VSEL pin is
	 * driven high -- see the header's @par regulator-map note. */
	[PCA9451A_RAIL_LDO5] = { PCA9451A_REG_LDO5CTRL_H,
	                         0x0F,
	                         PCA9451A_REG_LDO5CTRL_H,
	                         0xC0,
	                         RAIL_KIND_LDO5 },
};

static alp_status_t reg_read(pca9451a_t *ctx, uint8_t reg, uint8_t *out)
{
	return alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, out, 1);
}

static alp_status_t reg_write(pca9451a_t *ctx, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	return alp_i2c_write(ctx->bus, ctx->addr, buf, sizeof(buf));
}

/* code -> microvolts, per the rail's linear-range kind. */
static int32_t vsel_to_uv(rail_kind_t kind, uint8_t code)
{
	switch (kind) {
	case RAIL_KIND_DVS_BUCK:
		return 600000 + (int32_t)code * 12500;
	case RAIL_KIND_STD_BUCK:
		return (code >= 0x71) ? 3400000 : 600000 + (int32_t)code * 25000;
	case RAIL_KIND_LDO1:
		return (code <= 0x03) ? 1600000 + (int32_t)code * 100000
		                      : 3000000 + (int32_t)(code - 4) * 100000;
	case RAIL_KIND_LDO2:
		return 800000 + (int32_t)code * 50000;
	case RAIL_KIND_LDO34:
		return (code >= 0x1A) ? 3300000 : 800000 + (int32_t)code * 100000;
	case RAIL_KIND_LDO5:
		return 1800000 + (int32_t)code * 100000;
	default:
		return 0;
	}
}

/* microvolts -> code, per the rail's linear-range kind.  Returns
 * ALP_ERR_OUT_OF_RANGE if uv is below the rail's documented floor;
 * values above the ceiling clamp to the highest representable code
 * (matching the upstream driver's own fixed-tail linear_range
 * entries for BUCK4-6 / LDO3-4, and the natural saturation of a
 * single-range rail for the others). */
static alp_status_t uv_to_vsel(rail_kind_t kind, int32_t uv, uint8_t *code)
{
	switch (kind) {
	case RAIL_KIND_DVS_BUCK:
		if (uv < 600000) return ALP_ERR_OUT_OF_RANGE;
		*code = (uv - 600000) / 12500 > 0x7F ? 0x7F : (uint8_t)((uv - 600000) / 12500);
		return ALP_OK;
	case RAIL_KIND_STD_BUCK:
		if (uv < 600000) return ALP_ERR_OUT_OF_RANGE;
		*code = (uv >= 3400000) ? 0x71 : (uint8_t)((uv - 600000) / 25000);
		return ALP_OK;
	case RAIL_KIND_LDO1:
		if (uv < 1600000) return ALP_ERR_OUT_OF_RANGE;
		if (uv <= 1900000) {
			*code = (uint8_t)((uv - 1600000) / 100000);
		} else if (uv >= 3000000) {
			int32_t c = 4 + (uv - 3000000) / 100000;
			*code     = (uint8_t)(c > 0x07 ? 0x07 : c);
		} else {
			/* Gap between the two supported bands (1.9V..3.0V is not
			 * representable) -- pick whichever band edge is closer. */
			*code = (uv - 1900000 <= 3000000 - uv) ? 0x03 : 0x04;
		}
		return ALP_OK;
	case RAIL_KIND_LDO2: {
		if (uv < 800000) return ALP_ERR_OUT_OF_RANGE;
		int32_t c = (uv - 800000) / 50000;
		*code     = (uint8_t)(c > 0x07 ? 0x07 : c);
		return ALP_OK;
	}
	case RAIL_KIND_LDO34:
		if (uv < 800000) return ALP_ERR_OUT_OF_RANGE;
		*code = (uv >= 3300000) ? 0x1A : (uint8_t)((uv - 800000) / 100000);
		return ALP_OK;
	case RAIL_KIND_LDO5: {
		if (uv < 1800000) return ALP_ERR_OUT_OF_RANGE;
		int32_t c = (uv - 1800000) / 100000;
		*code     = (uint8_t)(c > 0x0F ? 0x0F : c);
		return ALP_OK;
	}
	default:
		return ALP_ERR_INVAL;
	}
}

alp_status_t pca9451a_init_at(pca9451a_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit)
{
	if (ctx == NULL || bus == NULL || addr_7bit == 0) return ALP_ERR_INVAL;

	memset(ctx, 0, sizeof(*ctx));
	ctx->bus  = bus;
	ctx->addr = addr_7bit;

	/* ACK-only probe -- see pca9451a_init's Doxygen for why this
	 * doesn't assert an expected DEV_ID value. */
	uint8_t dev_id = 0;
	if (reg_read(ctx, PCA9451A_REG_DEV_ID, &dev_id) != ALP_OK) {
		return ALP_ERR_NOT_READY;
	}

	ctx->dev_id      = dev_id;
	ctx->initialised = true;
	return ALP_OK;
}

alp_status_t pca9451a_init(pca9451a_t *ctx, alp_i2c_t *bus)
{
	return pca9451a_init_at(ctx, bus, PCA9451A_I2C_ADDR);
}

alp_status_t pca9451a_get_status(pca9451a_t *ctx, pca9451a_status_t *out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;

	uint8_t      reg = 0;
	alp_status_t s   = reg_read(ctx, PCA9451A_REG_INT1, &reg);
	if (s != ALP_OK) return s;

	out->raw       = reg;
	out->pwron     = (reg & PCA9451A_INT1_PWRON) != 0;
	out->wdogb     = (reg & PCA9451A_INT1_WDOGB) != 0;
	out->vr_flt1   = (reg & PCA9451A_INT1_VR_FLT1) != 0;
	out->vr_flt2   = (reg & PCA9451A_INT1_VR_FLT2) != 0;
	out->low_vsys  = (reg & PCA9451A_INT1_LOWVSYS) != 0;
	out->therm_105 = (reg & PCA9451A_INT1_THERM_105) != 0;
	out->therm_125 = (reg & PCA9451A_INT1_THERM_125) != 0;
	return ALP_OK;
}

alp_status_t pca9451a_rail_set_enable(pca9451a_t *ctx, pca9451a_rail_t rail, bool enable)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if ((unsigned)rail >= PCA9451A_RAIL_COUNT) return ALP_ERR_INVAL;

	const struct rail_loc *loc = &rail_table[rail];
	uint8_t                reg = 0;
	alp_status_t           s   = reg_read(ctx, loc->en_reg, &reg);
	if (s != ALP_OK) return s;

	reg = (uint8_t)((reg & ~loc->en_mask) | (enable ? loc->en_mask : 0));
	return reg_write(ctx, loc->en_reg, reg);
}

alp_status_t pca9451a_rail_is_enabled(pca9451a_t *ctx, pca9451a_rail_t rail, bool *enabled)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (enabled == NULL) return ALP_ERR_INVAL;
	if ((unsigned)rail >= PCA9451A_RAIL_COUNT) return ALP_ERR_INVAL;

	const struct rail_loc *loc = &rail_table[rail];
	uint8_t                reg = 0;
	alp_status_t           s   = reg_read(ctx, loc->en_reg, &reg);
	if (s != ALP_OK) return s;

	*enabled = (reg & loc->en_mask) != 0;
	return ALP_OK;
}

alp_status_t pca9451a_rail_set_voltage_uv(pca9451a_t *ctx, pca9451a_rail_t rail, int32_t microvolts)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if ((unsigned)rail >= PCA9451A_RAIL_COUNT) return ALP_ERR_INVAL;

	const struct rail_loc *loc  = &rail_table[rail];
	uint8_t                code = 0;
	alp_status_t           s    = uv_to_vsel(loc->kind, microvolts, &code);
	if (s != ALP_OK) return s;

	/* Read-modify-write: LDO vsel + enable fields share one register. */
	uint8_t reg = 0;
	s           = reg_read(ctx, loc->vsel_reg, &reg);
	if (s != ALP_OK) return s;

	reg = (uint8_t)((reg & ~loc->vsel_mask) | (code & loc->vsel_mask));
	return reg_write(ctx, loc->vsel_reg, reg);
}

alp_status_t
pca9451a_rail_get_voltage_uv(pca9451a_t *ctx, pca9451a_rail_t rail, int32_t *microvolts)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (microvolts == NULL) return ALP_ERR_INVAL;
	if ((unsigned)rail >= PCA9451A_RAIL_COUNT) return ALP_ERR_INVAL;

	const struct rail_loc *loc = &rail_table[rail];
	uint8_t                reg = 0;
	alp_status_t           s   = reg_read(ctx, loc->vsel_reg, &reg);
	if (s != ALP_OK) return s;

	*microvolts = vsel_to_uv(loc->kind, reg & loc->vsel_mask);
	return ALP_OK;
}

alp_status_t pca9451a_read_reg(pca9451a_t *ctx, uint8_t reg, uint8_t *out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;
	return reg_read(ctx, reg, out);
}

alp_status_t pca9451a_write_reg(pca9451a_t *ctx, uint8_t reg, uint8_t val)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	return reg_write(ctx, reg, val);
}

void pca9451a_deinit(pca9451a_t *ctx)
{
	if (ctx == NULL) return;
	ctx->initialised = false;
	ctx->bus         = NULL;
}
