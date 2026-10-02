/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * TI INA228 power / energy / charge monitor driver.
 * See <alp/chips/ina228.h> for the public API.
 *
 * ADR 0017: upstream Zephyr's `ti,ina228` driver is the path for a bus a
 * Zephyr core masters.  This portable driver covers the Linux i2c-dev path
 * on the E1M-X EVK, where the kernel has no INA228 support.
 *
 * Wire format: every register is big-endian.  A write is [reg, hi, lo]; a
 * read is [reg] followed by a 2-, 3- or 5-byte read, most significant byte
 * first (VSHUNT / VBUS / CURRENT / POWER are 24-bit, ENERGY / CHARGE 40-bit).
 *
 * Datasheet: SLYS021A (INA228, May 2022).
 */

#include <stddef.h>
#include <string.h>
#include <stdint.h>

#include "alp/chips/ina228.h"

/* Register map, SLYS021A table 7-3. */
#define INA228_REG_CONFIG     0x00u
#define INA228_REG_ADC_CONFIG 0x01u
#define INA228_REG_SHUNT_CAL  0x02u
#define INA228_REG_VSHUNT     0x04u
#define INA228_REG_VBUS       0x05u
#define INA228_REG_DIETEMP    0x06u
#define INA228_REG_CURRENT    0x07u
#define INA228_REG_POWER      0x08u
#define INA228_REG_ENERGY     0x09u
#define INA228_REG_CHARGE     0x0Au
#define INA228_REG_DIAG_ALRT  0x0Bu
#define INA228_REG_MFG_ID     0x3Eu
#define INA228_REG_DEVICE_ID  0x3Fu

/* CONFIG bits, table 7-5. */
#define INA228_CFG_RST          0x8000u /* Bit 15: reset, self-clearing. */
#define INA228_CFG_RSTACC       0x4000u /* Bit 14: clear ENERGY + CHARGE. */
#define INA228_CFG_ADCRANGE_BIT 4u      /* Bit 4: 0 = +/-163.84 mV, 1 = +/-40.96 mV. */
#define INA228_CFG_ADCRANGE     (1u << INA228_CFG_ADCRANGE_BIT)

/* ADC_CONFIG fields, table 7-6. */
#define INA228_ADC_MODE_SHIFT   12u
#define INA228_ADC_VBUSCT_SHIFT 9u
#define INA228_ADC_VSHCT_SHIFT  6u
#define INA228_ADC_VTCT_SHIFT   3u
#define INA228_ADC_AVG_SHIFT    0u

/* SHUNT_CAL is bits 14:0 (bit 15 reserved), table 7-7. */
#define INA228_SHUNT_CAL_MAX 0x7FFFu

/* Shunt full-scale voltage in micro-volts per ADCRANGE, table 7-5. */
#define INA228_FS_UV_163MV 163840u
#define INA228_FS_UV_40MV  40960u

/* CURRENT_LSB = max current / 2^19 (eq. 3). */
#define INA228_CURRENT_BITS 19u

/*
 * SHUNT_CAL = 13107.2e6 * CURRENT_LSB[A] * R_SHUNT[ohm] (eq. 2).  With
 * CURRENT_LSB in pico-amps and R in micro-ohms that is
 *   cal = lsb_pa * r_uohm * 13107.2e6 * 1e-12 * 1e-6 = lsb_pa * r_uohm * 16 / 5^13,
 * and 5^13 = 1220703125.  The factor 16 becomes 64 for ADCRANGE = 1.
 */
#define INA228_CAL_DIV 1220703125u

/* Pico-amps per micro-amp. */
#define INA228_PA_PER_UA 1000000ull

static alp_status_t reg_read(ina228_t *ctx, uint8_t reg, uint8_t *buf, size_t len)
{
	return alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, buf, len);
}

static alp_status_t reg_read16(ina228_t *ctx, uint8_t reg, uint16_t *val_out)
{
	uint8_t      buf[2];
	alp_status_t s = reg_read(ctx, reg, buf, sizeof(buf));
	if (s != ALP_OK) return s;
	*val_out = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
	return ALP_OK;
}

static alp_status_t reg_write16(ina228_t *ctx, uint8_t reg, uint16_t val)
{
	uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };
	return alp_i2c_write(ctx->bus, ctx->addr, buf, sizeof(buf));
}

/* Read a 24-bit register as an unsigned value. */
static alp_status_t reg_read24(ina228_t *ctx, uint8_t reg, uint32_t *val_out)
{
	uint8_t      buf[3];
	alp_status_t s = reg_read(ctx, reg, buf, sizeof(buf));
	if (s != ALP_OK) return s;
	*val_out = ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2];
	return ALP_OK;
}

/* Read a 40-bit register as an unsigned value. */
static alp_status_t reg_read40(ina228_t *ctx, uint8_t reg, uint64_t *val_out)
{
	uint8_t      buf[5];
	alp_status_t s = reg_read(ctx, reg, buf, sizeof(buf));
	if (s != ALP_OK) return s;
	uint64_t v = 0;
	for (size_t i = 0; i < sizeof(buf); i++)
		v = (v << 8) | buf[i];
	*val_out = v;
	return ALP_OK;
}

/* VSHUNT / VBUS / CURRENT: value in bits 23:4, two's complement. */
static int32_t sign_extend20(uint32_t raw24)
{
	int32_t v = (int32_t)(raw24 >> 4);
	if (v & 0x80000) v -= 0x100000;
	return v;
}

static int64_t sign_extend40(uint64_t raw40)
{
	int64_t v = (int64_t)raw40;
	if (raw40 & (1ull << 39)) v -= (int64_t)(1ull << 40);
	return v;
}

/* Round-to-nearest signed division by a positive constant. */
static int64_t div_round(int64_t num, int64_t den)
{
	return (num >= 0) ? (num + den / 2) / den : -((-num + den / 2) / den);
}

/* round(a * b * num / den) for unsigned values, half away from zero, exact
 * and without a 128-bit type.  The product is built in 32-bit limbs (a * b is
 * 128 bits, times num up to 160), `den / 2` is added for the rounding, and a
 * limb-wise long division by `den` (< 2^32, so each step fits 64 bits) gives
 * the quotient.  Fails (returns false) only when the RESULT does not fit 64
 * bits; an intermediate product that is wider than 64 bits is fine. */
static bool mul_div_round_u64(uint64_t a, uint64_t b, uint32_t num, uint32_t den, uint64_t *out)
{
	uint32_t al[2] = { (uint32_t)a, (uint32_t)(a >> 32) };
	uint32_t bl[2] = { (uint32_t)b, (uint32_t)(b >> 32) };
	uint32_t p[5]  = { 0, 0, 0, 0, 0 };

	for (int i = 0; i < 2; i++) {
		uint64_t carry = 0;
		for (int j = 0; j < 2; j++) {
			uint64_t t = (uint64_t)al[i] * bl[j] + p[i + j] + carry;
			p[i + j]   = (uint32_t)t;
			carry      = t >> 32;
		}
		p[i + 2] = (uint32_t)carry;
	}

	uint64_t carry = 0;
	for (int k = 0; k < 4; k++) {
		uint64_t t = (uint64_t)p[k] * num + carry;
		p[k]       = (uint32_t)t;
		carry      = t >> 32;
	}
	p[4] = (uint32_t)carry;

	carry = den / 2u;
	for (int k = 0; k < 5; k++) {
		uint64_t t = (uint64_t)p[k] + carry;
		p[k]       = (uint32_t)t;
		carry      = t >> 32;
	}

	uint32_t q[5];
	uint64_t rem = 0;
	for (int k = 4; k >= 0; k--) {
		uint64_t cur = (rem << 32) | p[k];
		q[k]         = (uint32_t)(cur / den);
		rem          = cur % den;
	}
	if (q[4] != 0u || q[3] != 0u || q[2] != 0u) return false;
	*out = ((uint64_t)q[1] << 32) | q[0];
	return true;
}

/* The statuses a no-ACK (nothing answers at the address) produces:
 *  - Zephyr: i2c_write_read() returns -EIO -> ALP_ERR_IO (alp_errno.h).
 *  - Linux i2c-dev: an address NACK on I2C_RDWR is ENXIO -> ALP_ERR_NOT_READY;
 *    a NACK after the address (EREMOTEIO, and anything unmapped) falls to
 *    ALP_ERR_IO.
 * ALP_ERR_NOT_READY also covers ENODEV / ENOENT (adapter gone), which is
 * indistinguishable from here and reads as "no device" too.  Everything else
 * -- ALP_ERR_BUSY (EBUSY / EAGAIN), ALP_ERR_TIMEOUT, ALP_ERR_NOSUPPORT,
 * ALP_ERR_NOMEM -- says something other than "no one is there". */
static bool is_no_ack(alp_status_t s)
{
	return s == ALP_ERR_IO || s == ALP_ERR_NOT_READY;
}

static bool addr_in_strap_range(uint8_t addr)
{
	return addr >= INA228_ADDR_MIN && addr <= INA228_ADDR_MAX;
}

uint64_t ina228_full_scale_ua(uint32_t shunt_micro_ohms, ina228_adcrange_t adcrange)
{
	if (shunt_micro_ohms == 0u) return 0u;
	uint64_t fs_uv = (adcrange == INA228_ADCRANGE_40MV) ? INA228_FS_UV_40MV : INA228_FS_UV_163MV;
	/* uV / uOhm = A, so uA = uV * 1e6 / uOhm. */
	return (fs_uv * 1000000ull) / shunt_micro_ohms;
}

alp_status_t ina228_calibration_for(uint32_t          shunt_micro_ohms,
                                    uint32_t          max_current_ua,
                                    ina228_adcrange_t adcrange,
                                    uint16_t         *shunt_cal_out,
                                    uint64_t         *lsb_pa_out)
{
	if (shunt_cal_out == NULL || lsb_pa_out == NULL) return ALP_ERR_INVAL;
	if (shunt_micro_ohms == 0u) return ALP_ERR_INVAL;
	if (max_current_ua == 0u || max_current_ua > INA228_MAX_CURRENT_LIMIT_UA) return ALP_ERR_INVAL;

	/* CURRENT_LSB = max current / 2^19 in pico-amps, rounded to nearest;
	 * (>= 2 pA for any request >= 1 uA).  Not clamped to the range's full scale: a request
	 * beyond it only coarsens every reading (use ina228_full_scale_ua() to
	 * pick the value), but it is the caller's stated scale. */
	uint64_t lsb_pa =
	    ((uint64_t)max_current_ua * INA228_PA_PER_UA + (1ull << (INA228_CURRENT_BITS - 1))) >>
	    INA228_CURRENT_BITS;

	uint64_t mult = (adcrange == INA228_ADCRANGE_40MV) ? 64u : 16u;
	uint64_t cal;
	if (!mul_div_round_u64(lsb_pa, shunt_micro_ohms, (uint32_t)mult, INA228_CAL_DIV, &cal))
		return ALP_ERR_OUT_OF_RANGE;
	if (cal == 0u || cal > INA228_SHUNT_CAL_MAX) return ALP_ERR_OUT_OF_RANGE;

	/* Back-compute CURRENT_LSB from the register actually written; it is a
	 * 15-bit integer that rounds, and every current / power / energy /
	 * charge reading is off by exactly the ratio discarded if the requested
	 * LSB is kept instead. */
	uint64_t lsb_back = (cal * INA228_CAL_DIV + (mult * shunt_micro_ohms) / 2u) /
	                    (mult * (uint64_t)shunt_micro_ohms);
	*lsb_pa_out       = lsb_back;
	*shunt_cal_out    = (uint16_t)cal;
	return ALP_OK;
}

static alp_status_t apply_calibration(ina228_t *ctx)
{
	uint16_t     cal = 0;
	uint64_t     lsb = 0;
	alp_status_t s   = ina228_calibration_for(
	    ctx->shunt_micro_ohms, ctx->max_current_ua, ctx->adcrange, &cal, &lsb);
	if (s != ALP_OK) return s;
	ctx->shunt_cal      = cal;
	ctx->current_lsb_pa = lsb;
	return reg_write16(ctx, INA228_REG_SHUNT_CAL, cal);
}

alp_status_t ina228_init(ina228_t         *ctx,
                         alp_i2c_t        *bus,
                         uint8_t           addr_7bit,
                         uint32_t          shunt_micro_ohms,
                         uint32_t          max_current_ua,
                         ina228_adcrange_t adcrange)
{
	if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (!addr_in_strap_range(addr_7bit)) return ALP_ERR_INVAL;
	if (adcrange != INA228_ADCRANGE_163MV && adcrange != INA228_ADCRANGE_40MV) return ALP_ERR_INVAL;

	/* Validate calibration before touching the bus. */
	uint16_t     cal;
	uint64_t     lsb;
	alp_status_t s = ina228_calibration_for(shunt_micro_ohms, max_current_ua, adcrange, &cal, &lsb);
	if (s != ALP_OK) return s;

	memset(ctx, 0, sizeof(*ctx));
	ctx->bus              = bus;
	ctx->addr             = addr_7bit;
	ctx->shunt_micro_ohms = shunt_micro_ohms;
	ctx->max_current_ua   = max_current_ua;
	ctx->adcrange         = adcrange;

	/* The first read doubles as the presence test.  Only a no-ACK result
	 * means "absent" (e.g. a carrier without the bus-pin rework); any other
	 * bus failure (BUSY when a kernel driver holds the address, TIMEOUT on a
	 * hung bus, NOSUPPORT, ...) is a real fault and is returned as is. */
	uint16_t id;
	s = reg_read16(ctx, INA228_REG_MFG_ID, &id);
	if (is_no_ack(s)) return INA228_ERR_NOT_PRESENT;
	if (s != ALP_OK) return s;
	if (id != INA228_MFG_ID) return ALP_ERR_NOT_READY;

	/* DEVICE_ID is DIEID[15:4] + REV[3:0]; the revision nibble varies, so
	 * only the die identifier is an identity gate. */
	s = reg_read16(ctx, INA228_REG_DEVICE_ID, &id);
	if (s != ALP_OK) return s;
	if ((id >> 4) != INA228_DIE_ID) return ALP_ERR_NOT_READY;

	/* Read-modify-write CONFIG so CONVDLY / TEMPCOMP survive; clear RST and
	 * RSTACC (they are actions, not state). */
	s = reg_read16(ctx, INA228_REG_CONFIG, &ctx->config_cache);
	if (s != ALP_OK) return s;
	ctx->config_cache &= (uint16_t)~(INA228_CFG_RST | INA228_CFG_RSTACC | INA228_CFG_ADCRANGE);
	if (adcrange == INA228_ADCRANGE_40MV) ctx->config_cache |= INA228_CFG_ADCRANGE;
	s = reg_write16(ctx, INA228_REG_CONFIG, ctx->config_cache);
	if (s != ALP_OK) return s;

	s = apply_calibration(ctx);
	if (s != ALP_OK) return s;

	ctx->initialised = true;
	return ALP_OK;
}

alp_status_t ina228_configure(ina228_t     *ctx,
                              ina228_avg_t  avg,
                              ina228_ct_t   vbusct,
                              ina228_ct_t   vshct,
                              ina228_ct_t   vtct,
                              ina228_mode_t mode)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if ((unsigned)avg > 7u || (unsigned)vbusct > 7u || (unsigned)vshct > 7u ||
	    (unsigned)vtct > 7u || (unsigned)mode > 15u)
		return ALP_ERR_INVAL;

	uint16_t v = (uint16_t)(((unsigned)mode << INA228_ADC_MODE_SHIFT) |
	                        ((unsigned)vbusct << INA228_ADC_VBUSCT_SHIFT) |
	                        ((unsigned)vshct << INA228_ADC_VSHCT_SHIFT) |
	                        ((unsigned)vtct << INA228_ADC_VTCT_SHIFT) |
	                        ((unsigned)avg << INA228_ADC_AVG_SHIFT));
	return reg_write16(ctx, INA228_REG_ADC_CONFIG, v);
}

alp_status_t ina228_read_shunt_uv(ina228_t *ctx, int32_t *uv_out)
{
	if (ctx == NULL || !ctx->initialised || uv_out == NULL) return ALP_ERR_NOT_READY;
	uint32_t     raw;
	alp_status_t s = reg_read24(ctx, INA228_REG_VSHUNT, &raw);
	if (s != ALP_OK) return s;
	/* 312.5 nV = 5/16 uV; 78.125 nV = 5/64 uV. */
	int64_t den = (ctx->adcrange == INA228_ADCRANGE_40MV) ? 64 : 16;
	*uv_out     = (int32_t)div_round((int64_t)sign_extend20(raw) * 5, den);
	return ALP_OK;
}

alp_status_t ina228_read_bus_uv(ina228_t *ctx, int32_t *uv_out)
{
	if (ctx == NULL || !ctx->initialised || uv_out == NULL) return ALP_ERR_NOT_READY;
	uint32_t     raw;
	alp_status_t s = reg_read24(ctx, INA228_REG_VBUS, &raw);
	if (s != ALP_OK) return s;
	/* 195.3125 uV = 3125/16 uV. */
	*uv_out = (int32_t)div_round((int64_t)sign_extend20(raw) * 3125, 16);
	return ALP_OK;
}

alp_status_t ina228_read_temp_mdegc(ina228_t *ctx, int32_t *mdegc_out)
{
	if (ctx == NULL || !ctx->initialised || mdegc_out == NULL) return ALP_ERR_NOT_READY;
	uint16_t     raw;
	alp_status_t s = reg_read16(ctx, INA228_REG_DIETEMP, &raw);
	if (s != ALP_OK) return s;
	/* 7.8125 m degC = 125/16 m degC. */
	*mdegc_out = (int32_t)div_round((int64_t)(int16_t)raw * 125, 16);
	return ALP_OK;
}

alp_status_t ina228_read_current_ua(ina228_t *ctx, int32_t *ua_out)
{
	if (ctx == NULL || !ctx->initialised || ua_out == NULL) return ALP_ERR_NOT_READY;
	uint32_t     raw;
	alp_status_t s = reg_read24(ctx, INA228_REG_CURRENT, &raw);
	if (s != ALP_OK) return s;
	/* |raw| <= 2^19 and lsb_pa < 1.91e9, so the product fits int64. */
	*ua_out = (int32_t)div_round((int64_t)sign_extend20(raw) * (int64_t)ctx->current_lsb_pa,
	                             (int64_t)INA228_PA_PER_UA);
	return ALP_OK;
}

alp_status_t ina228_read_power_uw(ina228_t *ctx, uint64_t *uw_out)
{
	if (ctx == NULL || !ctx->initialised || uw_out == NULL) return ALP_ERR_NOT_READY;
	uint32_t     raw;
	alp_status_t s = reg_read24(ctx, INA228_REG_POWER, &raw);
	if (s != ALP_OK) return s;
	/* P[W] = 3.2 * lsb[A] * POWER, so uW = lsb_pa * POWER * 16 / 5e6. */
	if (!mul_div_round_u64(ctx->current_lsb_pa, raw, 16u, 5000000u, uw_out))
		return ALP_ERR_OUT_OF_RANGE;
	return ALP_OK;
}

alp_status_t ina228_read_energy_uj(ina228_t *ctx, uint64_t *uj_out)
{
	if (ctx == NULL || !ctx->initialised || uj_out == NULL) return ALP_ERR_NOT_READY;
	uint64_t     raw;
	alp_status_t s = reg_read40(ctx, INA228_REG_ENERGY, &raw);
	if (s != ALP_OK) return s;
	/* E[J] = 16 * 3.2 * lsb[A] * ENERGY, so uJ = lsb_pa * ENERGY * 256 / 5e6. */
	if (!mul_div_round_u64(ctx->current_lsb_pa, raw, 256u, 5000000u, uj_out))
		return ALP_ERR_OUT_OF_RANGE;
	return ALP_OK;
}

alp_status_t ina228_read_charge_uc(ina228_t *ctx, int64_t *uc_out)
{
	if (ctx == NULL || !ctx->initialised || uc_out == NULL) return ALP_ERR_NOT_READY;
	uint64_t     raw;
	alp_status_t s = reg_read40(ctx, INA228_REG_CHARGE, &raw);
	if (s != ALP_OK) return s;
	int64_t  v   = sign_extend40(raw);
	uint64_t mag = (v < 0) ? (uint64_t)(-v) : (uint64_t)v;
	uint64_t uc;
	/* C = lsb[A] * CHARGE, so uC = lsb_pa * CHARGE / 1e6. */
	if (!mul_div_round_u64(ctx->current_lsb_pa, mag, 1u, 1000000u, &uc) || uc > (uint64_t)INT64_MAX)
		return ALP_ERR_OUT_OF_RANGE;
	*uc_out = (v < 0) ? -(int64_t)uc : (int64_t)uc;
	return ALP_OK;
}

alp_status_t ina228_reset_accumulators(ina228_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	/* Set RSTACC on top of the cached CONFIG (so ADCRANGE is not clobbered),
	 * then write the cache back so the bit is clear whether or not the part
	 * self-clears it. */
	alp_status_t s =
	    reg_write16(ctx, INA228_REG_CONFIG, (uint16_t)(ctx->config_cache | INA228_CFG_RSTACC));
	if (s != ALP_OK) return s;
	return reg_write16(ctx, INA228_REG_CONFIG, ctx->config_cache);
}

alp_status_t ina228_read_diag(ina228_t *ctx, uint16_t *flags_out)
{
	if (ctx == NULL || !ctx->initialised || flags_out == NULL) return ALP_ERR_NOT_READY;
	return reg_read16(ctx, INA228_REG_DIAG_ALRT, flags_out);
}

alp_status_t ina228_reset(ina228_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	alp_status_t s = reg_write16(ctx, INA228_REG_CONFIG, INA228_CFG_RST);
	if (s != ALP_OK) return s;
	/* All registers are back at their defaults (ADCRANGE = 0, CONVDLY = 0). */
	ctx->config_cache = 0u;
	if (ctx->adcrange == INA228_ADCRANGE_40MV) {
		ctx->config_cache = INA228_CFG_ADCRANGE;
		s                 = reg_write16(ctx, INA228_REG_CONFIG, ctx->config_cache);
		if (s != ALP_OK) return s;
	}
	return apply_calibration(ctx);
}

void ina228_deinit(ina228_t *ctx)
{
	if (ctx == NULL) return;
	memset(ctx, 0, sizeof(*ctx));
}
