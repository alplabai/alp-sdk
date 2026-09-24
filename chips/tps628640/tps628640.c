/*
 * Copyright 2026 Alp Lab AB
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
 *   0x03  CONTROL        operating-mode + ramp + reset
 *   0x05  STATUS    R    UVLO + HICCUP + thermal-warning latches (read clears)
 *
 * VOUT encoding (8-bit unsigned, 5 mV step starting at 400 mV):
 *   register_byte = (mv - 400) / 5
 *   range: 0x00 (400 mV) .. 0xFF (1675 mV)
 *
 * Guarded control: every write goes through the instance's
 * pmic_rail_limit_t entry installed with tps628640_set_limits().  No
 * entry => ALP_ERR_NOSUPPORT (fail-closed).  There is no raw write path.
 * Argument checks (NULL, chip range, enum range) run first, so a bad
 * argument reports ALP_ERR_INVAL / ALP_ERR_OUT_OF_RANGE even when no
 * entry is installed; nothing reaches the bus in either case.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/tps628640.h"

static alp_status_t reg_read(tps628640_t *ctx, uint8_t reg, uint8_t *val)
{
	return alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1u, val, 1u);
}

static alp_status_t reg_write(tps628640_t *ctx, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	return alp_i2c_write(ctx->bus, ctx->addr, buf, sizeof(buf));
}

static bool ready(const tps628640_t *ctx)
{
	return ctx != NULL && ctx->initialised;
}

/* Enable/disable permission shared by software_enable() and
 * reset_to_defaults() (a reset is a momentary rail drop). */
static alp_status_t check_enable(const tps628640_t *ctx, bool enable)
{
	const pmic_rail_limit_t *l = ctx->limit;
	if (l == NULL || !l->enable_writable) return ALP_ERR_NOSUPPORT;
	if (!enable && l->critical) return ALP_ERR_NOSUPPORT;
	return ALP_OK;
}

/* Write CONTROL; the shadow only moves on success so a failed write
 * never leaves it describing a state the chip does not have. */
static alp_status_t write_control(tps628640_t *ctx, uint8_t val)
{
	alp_status_t s = reg_write(ctx, TPS628640_REG_CONTROL, val);
	if (s == ALP_OK) ctx->control_shadow = val;
	return s;
}

static alp_status_t set_vout(tps628640_t *ctx, uint8_t reg, uint16_t mv)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	if (mv < TPS628640_VOUT_BASE_MV || mv > TPS628640_VOUT_MAX_MV) return ALP_ERR_OUT_OF_RANGE;

	const pmic_rail_limit_t *l = ctx->limit;
	if (l == NULL || !l->voltage_writable) return ALP_ERR_NOSUPPORT;

	/* Round down to the 5 mV grid, then guard the value that would
	 * actually be programmed, not the requested one. */
	uint8_t  code    = (uint8_t)((mv - TPS628640_VOUT_BASE_MV) / TPS628640_VOUT_STEP_MV);
	uint16_t encoded = (uint16_t)(TPS628640_VOUT_BASE_MV + (uint32_t)code * TPS628640_VOUT_STEP_MV);
	if (encoded < l->min_mv || encoded > l->max_mv) return ALP_ERR_OUT_OF_RANGE;

	alp_status_t s = reg_write(ctx, reg, code);
	if (s != ALP_OK) return s;
	uint8_t back = 0;
	s            = reg_read(ctx, reg, &back);
	if (s != ALP_OK) return s;
	return back == code ? ALP_OK : ALP_ERR_IO;
}

static alp_status_t get_vout(tps628640_t *ctx, uint8_t reg, uint16_t *mv)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	if (mv == NULL) return ALP_ERR_INVAL;
	uint8_t      raw = 0;
	alp_status_t s   = reg_read(ctx, reg, &raw);
	if (s != ALP_OK) return s;
	*mv = (uint16_t)(TPS628640_VOUT_BASE_MV + (uint32_t)raw * TPS628640_VOUT_STEP_MV);
	return ALP_OK;
}

alp_status_t
tps628640_init(tps628640_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit, uint16_t default_voltage_mv)
{
	if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (addr_7bit > 0x7Fu) return ALP_ERR_INVAL;

	memset(ctx, 0, sizeof(*ctx)); /* also clears ->limit: fail-closed until set_limits */
	ctx->bus                = bus;
	ctx->addr               = addr_7bit;
	ctx->default_voltage_mv = default_voltage_mv;
	ctx->control_shadow     = TPS628640_CTRL_DEFAULT;

	/* ACK-probe via VOUT1 (the family has no ID register). */
	uint8_t v = 0;
	if (reg_read(ctx, TPS628640_REG_VOUT1, &v) != ALP_OK) return ALP_ERR_NOT_READY;

	/* Seed the shadow from the live CONTROL byte when it reads back
	 * plausibly (bench V2M103 reads 0x6F).  A converter that answers
	 * here is running, so a byte without SOFTWARE_ENABLE means CONTROL
	 * did not read back (0x00) -- keep the datasheet default. */
	uint8_t ctrl = 0;
	if (reg_read(ctx, TPS628640_REG_CONTROL, &ctrl) == ALP_OK &&
	    (ctrl & TPS628640_CTRL_SOFTWARE_ENABLE) != 0u) {
		ctx->control_shadow = (uint8_t)(ctrl & (uint8_t)~TPS628640_CTRL_RESET);
	}

	ctx->initialised = true;
	return ALP_OK;
}

alp_status_t tps628640_set_limits(tps628640_t *ctx, const pmic_rail_limit_t *limit)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	if (limit != NULL && limit->voltage_writable &&
	    (limit->min_mv > limit->max_mv || limit->min_mv < TPS628640_VOUT_BASE_MV ||
	     limit->max_mv > TPS628640_VOUT_MAX_MV)) {
		return ALP_ERR_INVAL;
	}
	ctx->limit = limit;
	return ALP_OK;
}

alp_status_t tps628640_set_voltage_mv(tps628640_t *ctx, uint16_t mv)
{
	return set_vout(ctx, TPS628640_REG_VOUT1, mv);
}

alp_status_t tps628640_get_voltage_mv(tps628640_t *ctx, uint16_t *mv)
{
	return get_vout(ctx, TPS628640_REG_VOUT1, mv);
}

alp_status_t tps628640_set_voltage2_mv(tps628640_t *ctx, uint16_t mv)
{
	return set_vout(ctx, TPS628640_REG_VOUT2, mv);
}

alp_status_t tps628640_get_voltage2_mv(tps628640_t *ctx, uint16_t *mv)
{
	return get_vout(ctx, TPS628640_REG_VOUT2, mv);
}

alp_status_t tps628640_get_status(tps628640_t *ctx, uint8_t *status_byte)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	if (status_byte == NULL) return ALP_ERR_INVAL;
	/* Read-and-clear: every latched bit resets after this read, so the
	 * byte is everything that happened since the previous read. */
	return reg_read(ctx, TPS628640_REG_STATUS, status_byte);
}

alp_status_t tps628640_software_enable(tps628640_t *ctx, bool enable)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	alp_status_t s = check_enable(ctx, enable);
	if (s != ALP_OK) return s;

	if (enable) {
		/* Enabling energizes the chip's CURRENTLY programmed VOUT1 --
		 * refuse instead of blindly turning on an unconfirmed setpoint
		 * (stale POR value, or a window that shrank after the last
		 * write).  VOUT1 always reads back once the chip answers on the
		 * bus at all (bench-confirmed on every populated instance,
		 * including 0x4F/DDR5_VDDQ_0V5 at 0x14 = 500 mV), so there is no
		 * "not populated yet" excuse to skip this. */
		const pmic_rail_limit_t *l = ctx->limit;
		if (l->voltage_writable) {
			uint16_t mv = 0;
			s           = get_vout(ctx, TPS628640_REG_VOUT1, &mv);
			if (s != ALP_OK) return s;
			if (mv < l->min_mv || mv > l->max_mv) return ALP_ERR_OUT_OF_RANGE;
		}
	}

	uint8_t v = enable ? (uint8_t)(ctx->control_shadow | TPS628640_CTRL_SOFTWARE_ENABLE)
	                   : (uint8_t)(ctx->control_shadow & (uint8_t)~TPS628640_CTRL_SOFTWARE_ENABLE);
	return write_control(ctx, v);
}

alp_status_t tps628640_set_fpwm_mode(tps628640_t *ctx, bool fpwm)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	if (ctx->limit == NULL) return ALP_ERR_NOSUPPORT;
	uint8_t v = fpwm ? (uint8_t)(ctx->control_shadow | TPS628640_CTRL_FPWM_MODE)
	                 : (uint8_t)(ctx->control_shadow & (uint8_t)~TPS628640_CTRL_FPWM_MODE);
	return write_control(ctx, v);
}

alp_status_t tps628640_set_ramp_speed(tps628640_t *ctx, tps628640_ramp_speed_t speed)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	if ((unsigned)speed > 3u) return ALP_ERR_INVAL;
	if (ctx->limit == NULL) return ALP_ERR_NOSUPPORT;
	uint8_t v = (uint8_t)((ctx->control_shadow & (uint8_t)~TPS628640_CTRL_RAMP_SPEED_MASK) |
	                      ((unsigned)speed & TPS628640_CTRL_RAMP_SPEED_MASK));
	return write_control(ctx, v);
}

alp_status_t tps628640_reset_to_defaults(tps628640_t *ctx)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	alp_status_t s = check_enable(ctx, false);
	if (s != ALP_OK) return s;

	/* The chip's own reset re-enables the converter (CTRL_DEFAULT has
	 * SOFTWARE_ENABLE=1) at the datasheet-default FPWM/ramp settings
	 * (FPWM off, slowest ramp) -- silently dropping whatever a prior
	 * tps628640_set_fpwm_mode()/set_ramp_speed() call configured right
	 * as the rail re-energizes.  Capture them before the reset and
	 * restore them after, through the same guard check_enable() already
	 * cleared (ctx->limit is non-NULL whenever check_enable() succeeds),
	 * so e.g. a DEEPX buck that needs FPWM for load-transient stability
	 * doesn't come back up in PFM mode. */
	const uint8_t fpwm_bit  = (uint8_t)(ctx->control_shadow & TPS628640_CTRL_FPWM_MODE);
	const uint8_t ramp_bits = (uint8_t)(ctx->control_shadow & TPS628640_CTRL_RAMP_SPEED_MASK);

	/* One-shot reset bit: the chip reverts every register to its
	 * datasheet default and self-clears the bit. */
	s = reg_write(ctx, TPS628640_REG_CONTROL, TPS628640_CTRL_DEFAULT | TPS628640_CTRL_RESET);
	if (s != ALP_OK) return s;
	ctx->control_shadow = TPS628640_CTRL_DEFAULT;

	const uint8_t want =
	    (uint8_t)((ctx->control_shadow &
	               (uint8_t)~(TPS628640_CTRL_FPWM_MODE | TPS628640_CTRL_RAMP_SPEED_MASK)) |
	              fpwm_bit | ramp_bits);
	if (want != ctx->control_shadow) s = write_control(ctx, want);
	return s;
}

alp_status_t tps628640_read_reg(tps628640_t *ctx, uint8_t reg, uint8_t *val)
{
	if (!ready(ctx)) return ALP_ERR_NOT_READY;
	if (val == NULL) return ALP_ERR_INVAL;
	return reg_read(ctx, reg, val);
}

void tps628640_deinit(tps628640_t *ctx)
{
	if (ctx == NULL) return;
	ctx->initialised = false;
	ctx->bus         = NULL;
	ctx->limit       = NULL;
}
