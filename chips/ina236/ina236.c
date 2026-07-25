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

#include <math.h>
#include <string.h>
#include <stdint.h>

#include "alp/chips/ina236.h"

/* Datasheet SBOSA81D section 7.6.1 (register map). */
#define INA236_REG_CONFIG      0x00u
#define INA236_REG_SHUNT       0x01u
#define INA236_REG_BUS         0x02u
#define INA236_REG_POWER       0x03u
#define INA236_REG_CURRENT     0x04u
#define INA236_REG_CALIBRATION 0x05u
#define INA236_REG_MASK_ENABLE 0x06u
#define INA236_REG_ALERT_LIMIT 0x07u
#define INA236_REG_MFG_ID      0x3Eu
#define INA236_REG_DEVICE_ID   0x3Fu

/* CONFIG bit fields (datasheet SBOSA81D table 7-4). */
#define INA236_CFG_RST           0x8000u /* Bit 15: soft reset, self-clearing. */
#define INA236_CFG_ADCRANGE_20MV 0x1000u /* Bit 12: 0 = ±81.92 mV, 1 = ±20.48 mV. */
#define INA236_CFG_AVG_SHIFT     9u      /* Bits 11-9:  AVG.    */
#define INA236_CFG_AVG_MASK      0x0E00u
#define INA236_CFG_VBUSCT_SHIFT  6u /* Bits 8-6:   VBUSCT. */
#define INA236_CFG_VBUSCT_MASK   0x01C0u
#define INA236_CFG_VSHCT_SHIFT   3u /* Bits 5-3:   VSHCT.  */
#define INA236_CFG_VSHCT_MASK    0x0038u
#define INA236_CFG_MODE_MASK     0x0007u /* Bits 2-0:   MODE.   */

/* Mask/Enable bit fields (datasheet SBOSA81D table 7-10). */
#define INA236_MSKEN_CVRF 0x0008u /* Bit 3 (RO): conversion-ready flag. */

/* SHUNT_CAL is CALIBRATION bits 14-0 (bit 15 reserved), datasheet
 * table 7-9 -- so the programmable maximum is 0x7FFF, not 0xFFFF. */
#define INA236_SHUNT_CAL_MAX 32767.0f

/* Shunt-voltage LSB depends on ADCRANGE (datasheet section 6.5,
 * "1 LSB step size"). */
#define INA236_SHUNT_LSB_NV_RANGE_81MV 2500 /* 2.5 uV */
#define INA236_SHUNT_LSB_NV_RANGE_20MV 625  /* 0.625 uV */

/* Shunt-voltage full scale in microvolts for each ADCRANGE (datasheet
 * table 7-4: 0b = ±81.92 mV, 1b = ±20.48 mV).  Divided by the shunt
 * resistance this is the largest current the part can measure at all. */
#define INA236_FS_UV_RANGE_81MV 81920.0f
#define INA236_FS_UV_RANGE_20MV 20480.0f

/* Bus-voltage LSB is fixed at 1.6 mV. */
#define INA236_BUS_LSB_UV 1600u

/* VBUSCT / VSHCT code -> conversion time in microseconds, indexed by the
 * 3-bit field value (datasheet table 7-4).  Both fields share the table. */
static const uint16_t INA236_CT_US[8] = { 140, 204, 332, 588, 1100, 2116, 4156, 8244 };

/* AVG code -> number of averaged conversions (datasheet table 7-4). */
static const uint16_t INA236_AVG_N[8] = { 1, 4, 16, 64, 128, 256, 512, 1024 };

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

/* 7-bit I2C address strap range (header's "Address strap" table):
 * INA236A A0-encoded 0x40..0x43, INA236B 0x48..0x4B.  Anything else --
 * including the 0x80..0xFF band an 8-bit-encoded address would land
 * in -- is rejected before it ever reaches the bus. */
static bool addr_in_strap_range(uint8_t addr)
{
	return (addr >= 0x40u && addr <= 0x43u) || (addr >= 0x48u && addr <= 0x4Bu);
}

float ina236_full_scale_a(float shunt_ohms, ina236_adcrange_t adcrange)
{
	if (!isfinite(shunt_ohms) || shunt_ohms <= 0.0f) return 0.0f;
	float fs_uv =
	    (adcrange == INA236_ADCRANGE_20MV) ? INA236_FS_UV_RANGE_20MV : INA236_FS_UV_RANGE_81MV;
	return (fs_uv / 1000000.0f) / shunt_ohms;
}

/* SHUNT_CAL = 0.00512 / (CURRENT_LSB * R_SHUNT)  per datasheet SBOSA81D
 * eq. 1, where 0.00512 is "an internal fixed value used to ensure scaling
 * is maintained properly".  CURRENT_LSB is in amps and is the eq.-2 minimum
 * for a full-scale 16-bit signed CURRENT register -- derived below from the
 * reporting scale, which is the requested max_current clamped to what this
 * ADCRANGE can actually measure.
 *
 * ADCRANGE=1 quarters the shunt LSB (2.5 uV -> 625 nV), so the raw SHUNT
 * register is 4x larger for the same physical shunt voltage.  Since
 * CURRENT = SHUNT x SHUNT_CAL internally, SHUNT_CAL must be divided by 4 to
 * keep CURRENT (and therefore POWER) correctly scaled -- SBOSA81D section
 * 8.1.2 states this explicitly under eq. 1 ("The value of SHUNT_CAL must be
 * divided by 4 for ADCRANGE = 1").  Omitting it reads current and power 4x
 * high in the 20.48 mV range. */
alp_status_t ina236_calibration_for(float             shunt_ohms,
                                    float             max_current_a,
                                    ina236_adcrange_t adcrange,
                                    uint16_t         *shunt_cal_out,
                                    float            *current_lsb_a_out)
{
	if (shunt_cal_out == NULL || current_lsb_a_out == NULL) return ALP_ERR_INVAL;
	if (!isfinite(shunt_ohms) || shunt_ohms <= 0.0f) return ALP_ERR_INVAL;
	if (!isfinite(max_current_a) || max_current_a <= 0.0f) return ALP_ERR_INVAL;

	/* CURRENT_LSB sets the REPORTING scale of the CURRENT and POWER
	 * registers; it cannot create resolution the shunt ADC does not have.
	 * The ADC's own step is the shunt LSB over R_SHUNT (2.5 uV or 625 nV
	 * per count), and it saturates at the ADCRANGE full scale -- so a
	 * reporting scale WIDER than that full scale spends register range on
	 * currents the hardware physically cannot measure, and reports every
	 * current it CAN measure more coarsely than the ADC resolved it.
	 *
	 * Worked example (this is not hypothetical -- it is the E1M EVK +5V
	 * rail, 20 mOhm, whose board data rates it at 4.0 A):
	 *   ADCRANGE=1 -> shunt full scale 20.48 mV -> 1.024 A, ADC step
	 *   625 nV/20 mOhm = 31.25 uA.  Deriving CURRENT_LSB from 4.0 A gives
	 *   122.07 uA -- 3.9x coarser than the ADC, on a scale 3.9x wider than
	 *   the shunt can reach.  Deriving it from 1.024 A gives exactly
	 *   31.25 uA: matched to the ADC, and no measurable current is lost.
	 *
	 * So clamp the derivation to the range's own full-scale current. Only
	 * the wasteful direction is clamped: a caller that asks for a TIGHTER
	 * scale than the range (a known-small load) keeps it, since that is a
	 * deliberate resolution-for-headroom trade the datasheet allows. */
	float fs_current_a = ina236_full_scale_a(shunt_ohms, adcrange);
	float scale_a      = (max_current_a < fs_current_a) ? max_current_a : fs_current_a;

	float range_div = (adcrange == INA236_ADCRANGE_20MV) ? 4.0f : 1.0f;
	float cal_f     = 0.00512f / ((scale_a / 32768.0f) * shunt_ohms) / range_div;
	/* scale_a and shunt_ohms are both finite and > 0 (checked above), so
	 * cal_f is finite here -- no NaN/Inf can reach the uint16_t cast. */
	if (cal_f > INA236_SHUNT_CAL_MAX) cal_f = INA236_SHUNT_CAL_MAX;
	if (cal_f < 0.0f) cal_f = 0.0f;
	/* ROUND, don't truncate.  Truncation biases every reading low, and in
	 * float32 it does so even in the exactly-representable cases: the
	 * ADC-matched +5V configuration (20 mOhm, 20.48 mV range) computes
	 * 8191.9995 rather than 8192 because 20480/1e6 is not exact in float,
	 * so a cast would program 2047 instead of 2048 and skew CURRENT_LSB by
	 * 0.05 %. Half-up rounding is safe against the ceiling because cal_f was
	 * already clamped to INA236_SHUNT_CAL_MAX above. */
	uint16_t cal = (uint16_t)(cal_f + 0.5f);
	/* SHUNT_CAL is bounded BELOW by 2048 for every valid input, so it can
	 * never be 0 here: the reporting scale is clamped to the range's full
	 * scale FS/R, hence (scale/32768) * R <= FS/32768, hence
	 * cal >= 0.00512 * 32768 / FS / range_div = 2048 for both ranges (the
	 * ADC-matched point).  The guard stays because the next line divides by
	 * `cal`, and a SHUNT_CAL of 0 would mean "report zero current and
	 * power" (datasheet §7.6.1.6) rather than an error the caller could
	 * see -- it is a division guard, not a reachable input path. */
	if (cal == 0u) return ALP_ERR_INVAL;

	/* Back-compute CURRENT_LSB from the SHUNT_CAL actually written, never
	 * from the value we wanted.  The register is a 15-bit integer, so it
	 * both rounds and saturates; keeping the requested LSB after either
	 * would make every current and power read wrong by exactly the ratio
	 * we discarded, while still returning ALP_OK.  Saturation is reachable
	 * through the documented "narrower scale than the range" path -- e.g.
	 * a 20 mOhm shunt with max_current_a = 0.2 A wants SHUNT_CAL 41943,
	 * clamps to 32767, and would otherwise read 21.9 % low. Deriving the
	 * LSB from the register keeps the two consistent by construction. */
	*current_lsb_a_out = 0.00512f / ((float)cal * range_div * shunt_ohms);
	*shunt_cal_out     = cal;
	return ALP_OK;
}

/* Thin wrapper: the arithmetic above is a pure function so it can be unit
 * tested without a bus (the chips fake I2C layer is dormant by design), and
 * production takes the same path -- a test cannot drift from what ships. */
static alp_status_t apply_calibration(ina236_t *ctx)
{
	uint16_t     cal = 0;
	float        lsb = 0.0f;
	alp_status_t s =
	    ina236_calibration_for(ctx->shunt_ohms, ctx->max_current_a, ctx->adcrange, &cal, &lsb);
	if (s != ALP_OK) return s;

	ctx->full_scale_a  = ina236_full_scale_a(ctx->shunt_ohms, ctx->adcrange);
	ctx->current_lsb_a = lsb;
	return reg_write16(ctx, INA236_REG_CALIBRATION, cal);
}

alp_status_t ina236_init(ina236_t         *ctx,
                         alp_i2c_t        *bus,
                         uint8_t           addr_7bit,
                         float             shunt_ohms,
                         float             max_current_a,
                         ina236_adcrange_t adcrange)
{
	if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (!isfinite(shunt_ohms) || shunt_ohms <= 0.0f) return ALP_ERR_INVAL;
	if (!isfinite(max_current_a) || max_current_a <= 0.0f) return ALP_ERR_INVAL;
	/* addr_7bit == 0 is the documented "fall back to INA236A default"
	 * sentinel; any other value must fall inside the strap-encoded
	 * address range before it reaches the bus. */
	if (addr_7bit != 0 && !addr_in_strap_range(addr_7bit)) return ALP_ERR_INVAL;
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
	/* DEVICE_ID (0x3F) packs DIEID[15:4] + REV[3:0], which varies across INA236
	 * silicon lots/revisions -- e.g. 0xA480 read on the E1M-AEN801 bench vs the
	 * 0xA080 some early datasheets cite. It is therefore NOT a reliable identity
	 * gate; the MFG_ID (0x5449 = "TI") checked above is. Read it to confirm the
	 * register interface responds, but do not reject the part on its value. */
	s = reg_read16(ctx, INA236_REG_DEVICE_ID, &id);
	if (s != ALP_OK) return s;
	(void)id;

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
	/* SBOSA81D eq. 4: Power [W] = 32 x CURRENT_LSB x POWER.  The 32
     * is NOT dimensionless and NOT a bare count -- it already carries
     * the bus-voltage scaling (the internal register math divides by
     * 20000, and 20000 x 1.6 mV = 32 V), so the bus LSB must NOT be
     * applied a second time here.  Doing so under-reports power by
     * 625x; that was the shipped behaviour until this was checked
     * against eq. 4 (and against upstream Zephyr's
     * drivers/sensor/ti/ina2xx INA236_POWER_SCALING = 32, applied as
     * raw x current_lsb_uA x 32 -> uW). */
	float power_w = (float)raw * 32.0f * ctx->current_lsb_a;
	if (power_w < 0.0f) power_w = 0.0f;
	*uw_out = (uint32_t)(power_w * 1000000.0f);
	return ALP_OK;
}

alp_status_t ina236_read_power_raw(ina236_t *ctx, uint16_t *raw_out)
{
	if (ctx == NULL || !ctx->initialised || raw_out == NULL) return ALP_ERR_NOT_READY;
	return reg_read16(ctx, INA236_REG_POWER, raw_out);
}

float ina236_power_lsb_w(const ina236_t *ctx)
{
	/* SBOSA81D eq. 4, factored out so a high-rate sampler can stream raw
     * POWER counts and scale once on the host instead of per sample. */
	if (ctx == NULL) return 0.0f;
	return 32.0f * ctx->current_lsb_a;
}

alp_status_t ina236_configure(ina236_t     *ctx,
                              ina236_avg_t  avg,
                              ina236_ct_t   vbusct,
                              ina236_ct_t   vshct,
                              ina236_mode_t mode)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if ((unsigned)avg > 7u || (unsigned)vbusct > 7u || (unsigned)vshct > 7u || (unsigned)mode > 7u)
		return ALP_ERR_INVAL;

	/* Read-modify-write off the cache so RST (bit 15, self-clearing) and
     * ADCRANGE (bit 12, owned by init/reset) survive untouched -- and so
     * do the reserved bits 14-13, which always read back 10b. */
	uint16_t cfg = ctx->cfg_cache;
	cfg &= (uint16_t)~(INA236_CFG_AVG_MASK | INA236_CFG_VBUSCT_MASK | INA236_CFG_VSHCT_MASK |
	                   INA236_CFG_MODE_MASK);
	cfg |= (uint16_t)((unsigned)avg << INA236_CFG_AVG_SHIFT) & INA236_CFG_AVG_MASK;
	cfg |= (uint16_t)((unsigned)vbusct << INA236_CFG_VBUSCT_SHIFT) & INA236_CFG_VBUSCT_MASK;
	cfg |= (uint16_t)((unsigned)vshct << INA236_CFG_VSHCT_SHIFT) & INA236_CFG_VSHCT_MASK;
	cfg |= (uint16_t)(unsigned)mode & INA236_CFG_MODE_MASK;

	alp_status_t s = reg_write16(ctx, INA236_REG_CONFIG, cfg);
	if (s != ALP_OK) return s;
	ctx->cfg_cache = cfg;
	return ALP_OK;
}

uint16_t ina236_avg_count(ina236_avg_t avg)
{
	if ((unsigned)avg > 7u) return 0u;
	return INA236_AVG_N[(unsigned)avg];
}

uint16_t ina236_conversion_time_us(ina236_ct_t ct)
{
	if ((unsigned)ct > 7u) return 0u;
	return INA236_CT_US[(unsigned)ct];
}

uint32_t
ina236_sample_period_us(ina236_avg_t avg, ina236_ct_t vbusct, ina236_ct_t vshct, ina236_mode_t mode)
{
	if ((unsigned)avg > 7u || (unsigned)vbusct > 7u || (unsigned)vshct > 7u || (unsigned)mode > 7u)
		return 0u;

	uint32_t conv_us;
	switch (mode) {
	case INA236_MODE_SHUNT_CONT:
	case INA236_MODE_SHUNT_TRIG:
		conv_us = INA236_CT_US[(unsigned)vshct];
		break;
	case INA236_MODE_BUS_CONT:
	case INA236_MODE_BUS_TRIG:
		conv_us = INA236_CT_US[(unsigned)vbusct];
		break;
	case INA236_MODE_SHUNT_BUS_CONT:
	case INA236_MODE_SHUNT_BUS_TRIG:
		/* The ADC is multiplexed across both measurements (SBOSA81D
	     * section 7.3.1), so the two conversion times add. */
		conv_us = (uint32_t)INA236_CT_US[(unsigned)vbusct] + INA236_CT_US[(unsigned)vshct];
		break;
	default:
		/* Both Shutdown encodings (000b and 100b): no conversions. */
		return 0u;
	}
	return conv_us * INA236_AVG_N[(unsigned)avg];
}

alp_status_t ina236_conversion_ready(ina236_t *ctx, bool *ready_out)
{
	if (ctx == NULL || !ctx->initialised || ready_out == NULL) return ALP_ERR_NOT_READY;
	uint16_t     msken;
	alp_status_t s = reg_read16(ctx, INA236_REG_MASK_ENABLE, &msken);
	if (s != ALP_OK) return s;
	/* This read itself clears CVRF (SBOSA81D table 7-10, bit 3), which is
     * what makes the flag a one-shot consume-once handshake. */
	*ready_out = (msken & INA236_MSKEN_CVRF) != 0u;
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
