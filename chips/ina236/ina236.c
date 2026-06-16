/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * TI INA236 high-side current / bus-voltage / power monitor driver.
 * See <alp/chips/ina236.h> for the public API.
 *
 * Wire format: all registers are 16-bit big-endian on the I2C
 * bus -- a write transaction is [reg, hi, lo]; a read is [reg]
 * followed by a 2-byte read of [hi, lo].
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/ina236.h"

/* Datasheet SBOSA38A table 7-1. */
#define INA236_REG_CONFIG 0x00u
#define INA236_REG_SHUNT 0x01u
#define INA236_REG_BUS 0x02u
#define INA236_REG_POWER 0x03u
#define INA236_REG_CURRENT 0x04u
#define INA236_REG_CALIBRATION 0x05u
#define INA236_REG_MASK_ENABLE 0x06u
#define INA236_REG_ALERT_LIMIT 0x07u
#define INA236_REG_MFG_ID 0x3Eu
#define INA236_REG_DEVICE_ID 0x3Fu

/* CONFIG bit fields (datasheet table 7-2). */
#define INA236_CFG_RST 0x8000u           /* Soft reset.            */
#define INA236_CFG_ADCRANGE_20MV 0x1000u /* 0 = 81.92 mV, 1 = 20.48 mV. */

/* Shunt-voltage LSB depends on ADCRANGE (datasheet section 7.5.2). */
#define INA236_SHUNT_LSB_NV_RANGE_81MV 2500 /* 2.5 uV */
#define INA236_SHUNT_LSB_NV_RANGE_20MV 625  /* 0.625 uV */

/* Bus-voltage LSB is fixed at 1.6 mV. */
#define INA236_BUS_LSB_UV 1600u

static alp_status_t reg_read16(ina236_t *ctx, uint8_t reg, uint16_t *val_out)
{
	uint8_t      buf[2];
	alp_status_t s = alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, buf, 2);
	if (s != ALP_OK) return s;
	*val_out = ((uint16_t)buf[0] << 8) | buf[1];
	return ALP_OK;
}

static alp_status_t reg_write16(ina236_t *ctx, uint8_t reg, uint16_t val)
{
	uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };
	return alp_i2c_write(ctx->bus, ctx->addr, buf, sizeof(buf));
}

/* CALIBRATION = 0.00512 / (CURRENT_LSB * R_SHUNT)  per datasheet
 * eq. 1, where 0.00512 has units (V * A).  CURRENT_LSB is in
 * amps; we choose CURRENT_LSB = max_current / 32768. */
static alp_status_t apply_calibration(ina236_t *ctx)
{
	if (ctx->shunt_ohms <= 0.0f || ctx->max_current_a <= 0.0f) return ALP_ERR_INVAL;
	ctx->current_lsb_a = ctx->max_current_a / 32768.0f;
	float cal_f        = 0.00512f / (ctx->current_lsb_a * ctx->shunt_ohms);
	if (cal_f > 65535.0f) cal_f = 65535.0f;
	if (cal_f < 0.0f) cal_f = 0.0f;
	uint16_t cal = (uint16_t)cal_f;
	return reg_write16(ctx, INA236_REG_CALIBRATION, cal);
}

alp_status_t ina236_init(ina236_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit, float shunt_ohms,
                         float max_current_a, ina236_adcrange_t adcrange)
{
	if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (shunt_ohms <= 0.0f || max_current_a <= 0.0f) return ALP_ERR_INVAL;
	memset(ctx, 0, sizeof(*ctx));
	ctx->bus           = bus;
	ctx->addr          = (addr_7bit == 0) ? 0x40u : addr_7bit;
	ctx->shunt_ohms    = shunt_ohms;
	ctx->max_current_a = max_current_a;
	ctx->adcrange      = adcrange;

	/* Probe by reading the manufacturer + device ID -- guards
     * against silent address collisions on the shared sensor I2C
     * bus, where another chip might ACK a register read but
     * return garbage. */
	uint16_t     id;
	alp_status_t s = reg_read16(ctx, INA236_REG_MFG_ID, &id);
	if (s != ALP_OK) return ALP_ERR_NOT_READY;
	if (id != INA236_MFG_ID) return ALP_ERR_NOT_READY;
	s = reg_read16(ctx, INA236_REG_DEVICE_ID, &id);
	if (s != ALP_OK) return s;
	if (id != INA236_DEVICE_ID) return ALP_ERR_NOT_READY;

	/* Apply ADCRANGE to the configuration register; leave
     * conversion-time / averaging at reset defaults (continuous
     * mode, 1.1 ms conversion, no averaging) -- callers that
     * need lower-noise averaging can extend the API later. */
	s = reg_read16(ctx, INA236_REG_CONFIG, &ctx->cfg_cache);
	if (s != ALP_OK) return s;
	if (adcrange == INA236_ADCRANGE_20MV)
		ctx->cfg_cache |= INA236_CFG_ADCRANGE_20MV;
	else
		ctx->cfg_cache &= (uint16_t)~INA236_CFG_ADCRANGE_20MV;
	s = reg_write16(ctx, INA236_REG_CONFIG, ctx->cfg_cache);
	if (s != ALP_OK) return s;

	s = apply_calibration(ctx);
	if (s != ALP_OK) return s;

	ctx->initialised = true;
	return ALP_OK;
}

alp_status_t ina236_read_bus_mv(ina236_t *ctx, int32_t *mv_out)
{
	if (ctx == NULL || !ctx->initialised || mv_out == NULL) return ALP_ERR_NOT_READY;
	uint16_t     raw;
	alp_status_t s = reg_read16(ctx, INA236_REG_BUS, &raw);
	if (s != ALP_OK) return s;
	int16_t signed_raw = (int16_t)raw;
	/* LSB = 1.6 mV per bit -> mV = raw * 1.6.  Use integer math:
     * (raw * 16) / 10. */
	*mv_out = ((int32_t)signed_raw * 16) / 10;
	return ALP_OK;
}

alp_status_t ina236_read_shunt_uv(ina236_t *ctx, int32_t *uv_out)
{
	if (ctx == NULL || !ctx->initialised || uv_out == NULL) return ALP_ERR_NOT_READY;
	uint16_t     raw;
	alp_status_t s = reg_read16(ctx, INA236_REG_SHUNT, &raw);
	if (s != ALP_OK) return s;
	int16_t signed_raw = (int16_t)raw;
	/* nV per LSB depends on ADCRANGE; convert to uV. */
	int32_t lsb_nv = (ctx->adcrange == INA236_ADCRANGE_20MV) ? INA236_SHUNT_LSB_NV_RANGE_20MV
	                                                         : INA236_SHUNT_LSB_NV_RANGE_81MV;
	int64_t nv     = (int64_t)signed_raw * lsb_nv;
	*uv_out        = (int32_t)(nv / 1000);
	return ALP_OK;
}

alp_status_t ina236_read_current_ua(ina236_t *ctx, int32_t *ua_out)
{
	if (ctx == NULL || !ctx->initialised || ua_out == NULL) return ALP_ERR_NOT_READY;
	uint16_t     raw;
	alp_status_t s = reg_read16(ctx, INA236_REG_CURRENT, &raw);
	if (s != ALP_OK) return s;
	int16_t signed_raw = (int16_t)raw;
	/* CURRENT register reports raw * CURRENT_LSB amps.  Convert
     * to microamps via current_lsb_a * 1e6. */
	float current_a = (float)signed_raw * ctx->current_lsb_a;
	*ua_out         = (int32_t)(current_a * 1000000.0f);
	return ALP_OK;
}

alp_status_t ina236_read_power_uw(ina236_t *ctx, uint32_t *uw_out)
{
	if (ctx == NULL || !ctx->initialised || uw_out == NULL) return ALP_ERR_NOT_READY;
	uint16_t     raw;
	alp_status_t s = reg_read16(ctx, INA236_REG_POWER, &raw);
	if (s != ALP_OK) return s;
	/* Power LSB = 32 * CURRENT_LSB (amps) * 1.6 mV (bus LSB)
     * = 32 * current_lsb_a * 0.0016 V.  Multiply by 1e6 for uW. */
	float power_w = (float)raw * 32.0f * ctx->current_lsb_a * 0.0016f;
	if (power_w < 0.0f) power_w = 0.0f;
	*uw_out = (uint32_t)(power_w * 1000000.0f);
	return ALP_OK;
}

alp_status_t ina236_read_all(ina236_t *ctx, ina236_sample_t *sample_out)
{
	if (sample_out == NULL) return ALP_ERR_INVAL;
	alp_status_t s = ina236_read_bus_mv(ctx, &sample_out->bus_mv);
	if (s != ALP_OK) return s;
	s = ina236_read_shunt_uv(ctx, &sample_out->shunt_uv);
	if (s != ALP_OK) return s;
	s = ina236_read_current_ua(ctx, &sample_out->current_ua);
	if (s != ALP_OK) return s;
	return ina236_read_power_uw(ctx, &sample_out->power_uw);
}

alp_status_t ina236_reset(ina236_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	/* Setting the RST bit returns all registers to defaults.  We
     * then re-apply ADCRANGE + calibration. */
	alp_status_t s = reg_write16(ctx, INA236_REG_CONFIG, INA236_CFG_RST);
	if (s != ALP_OK) return s;
	s = reg_read16(ctx, INA236_REG_CONFIG, &ctx->cfg_cache);
	if (s != ALP_OK) return s;
	if (ctx->adcrange == INA236_ADCRANGE_20MV) {
		ctx->cfg_cache |= INA236_CFG_ADCRANGE_20MV;
		s = reg_write16(ctx, INA236_REG_CONFIG, ctx->cfg_cache);
		if (s != ALP_OK) return s;
	}
	return apply_calibration(ctx);
}

void ina236_deinit(ina236_t *ctx)
{
	if (ctx == NULL) return;
	memset(ctx, 0, sizeof(*ctx));
}
