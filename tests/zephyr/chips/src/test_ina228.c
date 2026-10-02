/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * TI INA228 power / energy / charge monitor: identity probe, SHUNT_CAL
 * arithmetic, 20-bit / 40-bit sign extension, per-ADCRANGE scaling and the
 * big-endian wire format, driven against the fake i2c-emul target in
 * fake_ina228.c.  Expected values are worked out from SLYS021A in the
 * comments; the reference shunt is the E1M-X EVK's 100 mOhm.
 */

#include <errno.h>

#include <zephyr/ztest.h>

#include "alp/chips/ina228.h"
#include "alp/e1m_pinout.h"
#include "alp/peripheral.h"

#include "fakes.h"
#include "range_policy.h"

#define SHUNT_100MOHM_UOHM 100000u
/* 100 mOhm x 163.84 mV full scale = 1.6384 A. */
#define FS_1638MA_UA 1638400u

#define REG_CONFIG     0x00u
#define REG_ADC_CONFIG 0x01u
#define REG_SHUNT_CAL  0x02u
#define REG_VSHUNT     0x04u
#define REG_VBUS       0x05u
#define REG_DIETEMP    0x06u
#define REG_CURRENT    0x07u
#define REG_POWER      0x08u
#define REG_ENERGY     0x09u
#define REG_CHARGE     0x0Au
#define REG_DIAG_ALRT  0x0Bu
#define REG_MFG_ID     0x3Eu
#define REG_DEVICE_ID  0x3Fu

static alp_i2c_t *open_bus(void)
{
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	return bus;
}

/* Init on the reference shunt, full-scale reporting scale, ADCRANGE as given. */
static void init_ref(ina228_t *ctx, alp_i2c_t *bus, ina228_adcrange_t range)
{
	uint32_t max_ua = (range == INA228_ADCRANGE_40MV) ? FS_1638MA_UA / 4u : FS_1638MA_UA;
	zassert_equal(ina228_init(ctx, bus, 0x42u, SHUNT_100MOHM_UOHM, max_ua, range), ALP_OK);
}

ZTEST(alp_chips, test_ina228_init_rejects_bad_arguments)
{
	fake_ina228_reset();
	alp_i2c_t *bus = open_bus();
	ina228_t   ctx;

	zassert_equal(ina228_init(NULL, bus, 0x42u, 100000u, FS_1638MA_UA, INA228_ADCRANGE_163MV),
	              ALP_ERR_INVAL);
	zassert_equal(ina228_init(&ctx, NULL, 0x42u, 100000u, FS_1638MA_UA, INA228_ADCRANGE_163MV),
	              ALP_ERR_INVAL);
	/* Zero shunt, zero / over-limit current, address outside 0x40..0x4F. */
	zassert_equal(ina228_init(&ctx, bus, 0x42u, 0u, FS_1638MA_UA, INA228_ADCRANGE_163MV),
	              ALP_ERR_INVAL);
	zassert_equal(ina228_init(&ctx, bus, 0x42u, 100000u, 0u, INA228_ADCRANGE_163MV), ALP_ERR_INVAL);
	zassert_equal(
	    ina228_init(
	        &ctx, bus, 0x42u, 100000u, INA228_MAX_CURRENT_LIMIT_UA + 1u, INA228_ADCRANGE_163MV),
	    ALP_ERR_INVAL);
	zassert_equal(ina228_init(&ctx, bus, 0x3Fu, 100000u, FS_1638MA_UA, INA228_ADCRANGE_163MV),
	              ALP_ERR_INVAL);
	zassert_equal(ina228_init(&ctx, bus, 0x50u, 100000u, FS_1638MA_UA, INA228_ADCRANGE_163MV),
	              ALP_ERR_INVAL);
	zassert_equal(ina228_init(&ctx, bus, 0x42u, 100000u, FS_1638MA_UA, (ina228_adcrange_t)2),
	              ALP_ERR_INVAL);
	/* Nothing above may have reached the bus. */
	zassert_equal(fake_ina228_log_len(), 0u);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ina228_init_distinguishes_absent_from_wrong_part)
{
	alp_i2c_t *bus = open_bus();
	ina228_t   ctx;

	/* Nothing acknowledges: "not present", a different status from a
	 * wrong-part mismatch, and not a generic bus error. */
	fake_ina228_reset();
	fake_ina228_set_absent(true);
	zassert_equal(ina228_init(&ctx, bus, 0x42u, 100000u, FS_1638MA_UA, INA228_ADCRANGE_163MV),
	              INA228_ERR_NOT_PRESENT);
	zassert_not_equal(INA228_ERR_NOT_PRESENT, ALP_ERR_NOT_READY);
	zassert_not_equal(INA228_ERR_NOT_PRESENT, ALP_ERR_IO);
	zassert_false(ctx.initialised);

	/* Only a no-ACK is "absent".  Linux i2c-dev reports an address NACK as
	 * ENXIO (-> ALP_ERR_NOT_READY); that is "absent" too. */
	fake_ina228_reset();
	fake_ina228_set_error(-ENXIO);
	zassert_equal(ina228_init(&ctx, bus, 0x42u, 100000u, FS_1638MA_UA, INA228_ADCRANGE_163MV),
	              INA228_ERR_NOT_PRESENT);

	/* Real bus faults are propagated, not folded into "absent": a kernel
	 * driver already bound at the address (EBUSY), a hung bus (ETIMEDOUT),
	 * a controller without the transfer type (ENOSYS), no memory (ENOMEM). */
	fake_ina228_reset();
	fake_ina228_set_error(-EBUSY);
	zassert_equal(ina228_init(&ctx, bus, 0x42u, 100000u, FS_1638MA_UA, INA228_ADCRANGE_163MV),
	              ALP_ERR_BUSY);
	fake_ina228_set_error(-ETIMEDOUT);
	zassert_equal(ina228_init(&ctx, bus, 0x42u, 100000u, FS_1638MA_UA, INA228_ADCRANGE_163MV),
	              ALP_ERR_TIMEOUT);
	fake_ina228_set_error(-ENOSYS);
	zassert_equal(ina228_init(&ctx, bus, 0x42u, 100000u, FS_1638MA_UA, INA228_ADCRANGE_163MV),
	              ALP_ERR_NOSUPPORT);
	fake_ina228_set_error(-ENOMEM);
	zassert_equal(ina228_init(&ctx, bus, 0x42u, 100000u, FS_1638MA_UA, INA228_ADCRANGE_163MV),
	              ALP_ERR_NOMEM);
	zassert_false(ctx.initialised);

	/* Wrong manufacturer ID. */
	fake_ina228_reset();
	fake_ina228_set_reg(REG_MFG_ID, 0x5450u);
	zassert_equal(ina228_init(&ctx, bus, 0x42u, 100000u, FS_1638MA_UA, INA228_ADCRANGE_163MV),
	              ALP_ERR_NOT_READY);
	zassert_false(ctx.initialised);

	/* Right manufacturer, wrong die ID (an INA226-style 0x2260). */
	fake_ina228_reset();
	fake_ina228_set_reg(REG_DEVICE_ID, 0x2260u);
	zassert_equal(ina228_init(&ctx, bus, 0x42u, 100000u, FS_1638MA_UA, INA228_ADCRANGE_163MV),
	              ALP_ERR_NOT_READY);
	zassert_equal(fake_ina228_write_count(REG_SHUNT_CAL), 0u, "no write after a failed probe");

	/* Any revision nibble passes: only DIEID[15:4] is the identity. */
	fake_ina228_reset();
	fake_ina228_set_reg(REG_DEVICE_ID, 0x228Fu);
	zassert_equal(ina228_init(&ctx, bus, 0x42u, 100000u, FS_1638MA_UA, INA228_ADCRANGE_163MV),
	              ALP_OK);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ina228_shunt_cal_programming)
{
	alp_i2c_t *bus = open_bus();
	ina228_t   ctx;

	/* ADCRANGE = 0: CURRENT_LSB = 1.6384 A / 2^19 = 3.125 uA, so
	 * SHUNT_CAL = 13107.2e6 x 3.125e-6 x 0.1 = 4096 (0x1000). */
	fake_ina228_reset();
	init_ref(&ctx, bus, INA228_ADCRANGE_163MV);
	zassert_equal(fake_ina228_get_reg(REG_SHUNT_CAL), 0x1000u);
	zassert_equal(ctx.shunt_cal, 4096u);
	zassert_equal(ctx.current_lsb_pa, 3125000u, "3.125 uA = 3,125,000 pA");
	zassert_equal(fake_ina228_get_reg(REG_CONFIG) & 0x0010u, 0u, "ADCRANGE bit clear");

	/* ADCRANGE = 1 (40.96 mV, 0.4096 A full scale): LSB = 781.25 nA and the
	 * eq.-2 value is multiplied by 4: 13107.2e6 x 781.25e-9 x 0.1 x 4 = 4096. */
	fake_ina228_reset();
	init_ref(&ctx, bus, INA228_ADCRANGE_40MV);
	zassert_equal(fake_ina228_get_reg(REG_SHUNT_CAL), 4096u);
	zassert_equal(ctx.current_lsb_pa, 781250u);
	zassert_equal(fake_ina228_get_reg(REG_CONFIG) & 0x0010u, 0x0010u, "ADCRANGE bit set");

	/* A request above the range's full scale is rejected (ina236 would clamp
	 * it), with nothing written to the part: 10 A on this shunt is 6.1x the
	 * 1.6384 A full scale. */
	fake_ina228_reset();
	zassert_equal(ina228_init(&ctx, bus, 0x42u, 100000u, 10000000u, INA228_ADCRANGE_163MV),
	              ALP_ERR_OUT_OF_RANGE);
	zassert_equal(fake_ina228_log_len(), 0u);
	/* ... and the full-scale value itself is accepted, 1 uA more is not. */
	zassert_equal(ina228_init(&ctx, bus, 0x42u, 100000u, 1638400u, INA228_ADCRANGE_163MV), ALP_OK);
	zassert_equal(ina228_init(&ctx, bus, 0x42u, 100000u, 1638401u, INA228_ADCRANGE_163MV),
	              ALP_ERR_OUT_OF_RANGE);
	zassert_equal(ina228_init(&ctx, bus, 0x42u, 100000u, 409600u, INA228_ADCRANGE_40MV), ALP_OK);
	zassert_equal(ina228_init(&ctx, bus, 0x42u, 100000u, 409601u, INA228_ADCRANGE_40MV),
	              ALP_ERR_OUT_OF_RANGE);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ina228_calibration_for_edges)
{
	uint16_t cal = 0;
	uint64_t lsb = 0;

	zassert_equal(ina228_calibration_for(100000u, FS_1638MA_UA, INA228_ADCRANGE_163MV, &cal, &lsb),
	              ALP_OK);
	zassert_equal(cal, 4096u);
	zassert_equal(lsb, 3125000u);

	/* NULL outputs, zero shunt, zero / over-limit current. */
	zassert_equal(ina228_calibration_for(100000u, FS_1638MA_UA, INA228_ADCRANGE_163MV, NULL, &lsb),
	              ALP_ERR_INVAL);
	zassert_equal(ina228_calibration_for(100000u, FS_1638MA_UA, INA228_ADCRANGE_163MV, &cal, NULL),
	              ALP_ERR_INVAL);
	zassert_equal(ina228_calibration_for(0u, FS_1638MA_UA, INA228_ADCRANGE_163MV, &cal, &lsb),
	              ALP_ERR_INVAL,
	              "zero shunt");
	zassert_equal(ina228_calibration_for(100000u, 0u, INA228_ADCRANGE_163MV, &cal, &lsb),
	              ALP_ERR_INVAL,
	              "zero max current");
	zassert_equal(ina228_calibration_for(
	                  100000u, INA228_MAX_CURRENT_LIMIT_UA + 1u, INA228_ADCRANGE_163MV, &cal, &lsb),
	              ALP_ERR_INVAL);

	/* Above the range's full scale is rejected, not clamped (ina236 clamps):
	 * 1.6384 A is the wide range's full scale on 100 mOhm. */
	zassert_equal(ina228_calibration_for(100000u, 1638400u, INA228_ADCRANGE_163MV, &cal, &lsb),
	              ALP_OK);
	zassert_equal(cal, 4096u);
	zassert_equal(ina228_calibration_for(100000u, 1638401u, INA228_ADCRANGE_163MV, &cal, &lsb),
	              ALP_ERR_OUT_OF_RANGE);
	zassert_equal(ina228_calibration_for(100000u, 20000000u, INA228_ADCRANGE_163MV, &cal, &lsb),
	              ALP_ERR_OUT_OF_RANGE);
	/* The narrow range's full scale is a quarter of that, and the same 4096
	 * SHUNT_CAL: x4 for the range, /4 for the LSB. */
	zassert_equal(ina228_calibration_for(100000u, 409600u, INA228_ADCRANGE_40MV, &cal, &lsb),
	              ALP_OK);
	zassert_equal(cal, 4096u);
	zassert_equal(lsb, 781250u);
	zassert_equal(ina228_calibration_for(100000u, 409601u, INA228_ADCRANGE_40MV, &cal, &lsb),
	              ALP_ERR_OUT_OF_RANGE);
	/* The same request on both ranges: SHUNT_CAL is 4x on the narrow one. */
	uint16_t cal_wide = 0, cal_narrow = 0;
	zassert_equal(ina228_calibration_for(100000u, 400000u, INA228_ADCRANGE_163MV, &cal_wide, &lsb),
	              ALP_OK);
	zassert_equal(ina228_calibration_for(100000u, 400000u, INA228_ADCRANGE_40MV, &cal_narrow, &lsb),
	              ALP_OK);
	zassert_true(cal_narrow >= 4u * cal_wide - 2u && cal_narrow <= 4u * cal_wide + 2u,
	             "wide %u narrow %u",
	             cal_wide,
	             cal_narrow);

	/* SHUNT_CAL that rounds to 0 is rejected too (it would report 0 A). */
	zassert_equal(ina228_calibration_for(1u, 1u, INA228_ADCRANGE_163MV, &cal, &lsb),
	              ALP_ERR_OUT_OF_RANGE);

	/* Full scale is (range voltage) / R. */
	zassert_equal(ina228_full_scale_ua(100000u, INA228_ADCRANGE_163MV), 1638400u);
	zassert_equal(ina228_full_scale_ua(100000u, INA228_ADCRANGE_40MV), 409600u);
	zassert_equal(ina228_full_scale_ua(0u, INA228_ADCRANGE_163MV), 0u);
}

ZTEST(alp_chips, test_ina228_shunt_voltage_scaling_and_sign)
{
	alp_i2c_t *bus = open_bus();
	ina228_t   ctx;
	int32_t    uv;

	/* The 20-bit value sits in bits 23:4 of the 24-bit register. */
	fake_ina228_reset();
	init_ref(&ctx, bus, INA228_ADCRANGE_163MV);
	fake_ina228_set_reg(REG_VSHUNT, 1000u << 4); /* 1000 x 312.5 nV = 312.5 uV -> 313 */
	zassert_equal(ina228_read_shunt_uv(&ctx, &uv), ALP_OK);
	zassert_equal(uv, 313);
	fake_ina228_set_reg(REG_VSHUNT, 0xFFFFF0u); /* -1 LSB = -312.5 nV -> 0 */
	zassert_equal(ina228_read_shunt_uv(&ctx, &uv), ALP_OK);
	zassert_equal(uv, 0);
	fake_ina228_set_reg(REG_VSHUNT, 0xFFC180u); /* -1000 LSB -> -312.5 uV -> -313 */
	zassert_equal(ina228_read_shunt_uv(&ctx, &uv), ALP_OK);
	zassert_equal(uv, -313);
	fake_ina228_set_reg(REG_VSHUNT, 0x800000u); /* most negative: -524288 x 312.5 nV */
	zassert_equal(ina228_read_shunt_uv(&ctx, &uv), ALP_OK);
	zassert_equal(uv, -163840);
	fake_ina228_set_reg(REG_VSHUNT, 0x7FFFF0u); /* most positive */
	zassert_equal(ina228_read_shunt_uv(&ctx, &uv), ALP_OK);
	zassert_equal(uv, 163840); /* 524287 x 312.5 nV = 163839.69 uV */

	/* ADCRANGE = 1: 78.125 nV/LSB, a quarter of the range-0 step. */
	fake_ina228_reset();
	init_ref(&ctx, bus, INA228_ADCRANGE_40MV);
	fake_ina228_set_reg(REG_VSHUNT, 1000u << 4); /* 78.125 uV -> 78 */
	zassert_equal(ina228_read_shunt_uv(&ctx, &uv), ALP_OK);
	zassert_equal(uv, 78);
	fake_ina228_set_reg(REG_VSHUNT, 0x800000u); /* -524288 x 78.125 nV = -40960 uV */
	zassert_equal(ina228_read_shunt_uv(&ctx, &uv), ALP_OK);
	zassert_equal(uv, -40960);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ina228_bus_voltage_and_temperature)
{
	alp_i2c_t *bus = open_bus();
	ina228_t   ctx;
	int32_t    v;

	fake_ina228_reset();
	init_ref(&ctx, bus, INA228_ADCRANGE_163MV);

	/* 5 V / 195.3125 uV = 25600 counts, in bits 23:4. */
	fake_ina228_set_reg(REG_VBUS, 25600u << 4);
	zassert_equal(ina228_read_bus_uv(&ctx, &v), ALP_OK);
	zassert_equal(v, 5000000);
	/* Full 20-bit scale: 0xFFFFF x 195.3125 uV is just under 204.8 V; the
	 * part never reports above 85 V but the register is two's complement
	 * "however always positive", so bit 19 set reads as a negative count. */
	fake_ina228_set_reg(REG_VBUS, 0x7FFFF0u);
	zassert_equal(ina228_read_bus_uv(&ctx, &v), ALP_OK);
	zassert_equal(v, 102399805); /* 524287 x 195.3125 uV = 102,399,804.7 */

	/* DIETEMP: 16-bit signed, 7.8125 m degC/LSB.  3200 -> 25.000 degC. */
	fake_ina228_set_reg(REG_DIETEMP, 3200u);
	zassert_equal(ina228_read_temp_mdegc(&ctx, &v), ALP_OK);
	zassert_equal(v, 25000);
	fake_ina228_set_reg(REG_DIETEMP, 0xFFFFu - 3200u + 1u); /* -3200 -> -25.000 degC */
	zassert_equal(ina228_read_temp_mdegc(&ctx, &v), ALP_OK);
	zassert_equal(v, -25000);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ina228_current_sign_extension)
{
	alp_i2c_t *bus = open_bus();
	ina228_t   ctx;
	int32_t    ua;

	fake_ina228_reset();
	init_ref(&ctx, bus, INA228_ADCRANGE_163MV); /* CURRENT_LSB = 3.125 uA */

	fake_ina228_set_reg(REG_CURRENT, 320000u << 4); /* 320000 x 3.125 uA = 1.0 A */
	zassert_equal(ina228_read_current_ua(&ctx, &ua), ALP_OK);
	zassert_equal(ua, 1000000);

	/* Negative current: two's complement over 20 bits. */
	fake_ina228_set_reg(REG_CURRENT, 0xFFFFF0u); /* -1 count */
	zassert_equal(ina228_read_current_ua(&ctx, &ua), ALP_OK);
	zassert_equal(ua, -3);                       /* -3.125 uA */
	fake_ina228_set_reg(REG_CURRENT, 0xB1E000u); /* -320000 counts = -1.0 A */
	zassert_equal(ina228_read_current_ua(&ctx, &ua), ALP_OK);
	zassert_equal(ua, -1000000);
	fake_ina228_set_reg(REG_CURRENT, 0x800000u); /* most negative: -2^19 x 3.125 uA */
	zassert_equal(ina228_read_current_ua(&ctx, &ua), ALP_OK);
	zassert_equal(ua, -1638400);
	fake_ina228_set_reg(REG_CURRENT, 0x7FFFF0u); /* most positive */
	zassert_equal(ina228_read_current_ua(&ctx, &ua), ALP_OK);
	zassert_equal(ua, 1638397); /* 524287 x 3.125 uA = 1,638,396.9 */

	/* The low nibble of the register is reserved and must not leak in. */
	fake_ina228_set_reg(REG_CURRENT, (320000u << 4) | 0xFu);
	zassert_equal(ina228_read_current_ua(&ctx, &ua), ALP_OK);
	zassert_equal(ua, 1000000);

	/* ADCRANGE = 1 shrinks the LSB to 781.25 nA: same register, a quarter the current. */
	fake_ina228_reset();
	init_ref(&ctx, bus, INA228_ADCRANGE_40MV);
	fake_ina228_set_reg(REG_CURRENT, 320000u << 4);
	zassert_equal(ina228_read_current_ua(&ctx, &ua), ALP_OK);
	zassert_equal(ua, 250000);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ina228_power_energy_charge)
{
	alp_i2c_t *bus = open_bus();
	ina228_t   ctx;
	uint64_t   u;
	int64_t    c;

	fake_ina228_reset();
	init_ref(&ctx, bus, INA228_ADCRANGE_163MV); /* CURRENT_LSB = 3.125 uA */

	/* Power [W] = 3.2 x CURRENT_LSB x POWER: 1,000,000 counts -> 10 W. */
	fake_ina228_set_reg(REG_POWER, 1000000u);
	zassert_equal(ina228_read_power_uw(&ctx, &u), ALP_OK);
	zassert_equal(u, 10000000u);
	fake_ina228_set_reg(REG_POWER, 0xFFFFFFu); /* 24-bit unsigned: no sign */
	zassert_equal(ina228_read_power_uw(&ctx, &u), ALP_OK);
	zassert_equal(u, 167772150u); /* 16777215 x 10 uW */

	/* Energy [J] = 16 x 3.2 x CURRENT_LSB x ENERGY: 1,000,000 -> 160 J. */
	fake_ina228_set_reg(REG_ENERGY, 1000000u);
	zassert_equal(ina228_read_energy_uj(&ctx, &u), ALP_OK);
	zassert_equal(u, 160000000u);
	/* All 40 bits set is an unsigned 1,099,511,627,775, not -1. */
	fake_ina228_set_reg(REG_ENERGY, 0xFFFFFFFFFFull);
	zassert_equal(ina228_read_energy_uj(&ctx, &u), ALP_OK);
	zassert_equal(u, 175921860444000ull); /* 1,099,511,627,775 x 160 uJ */

	/* Charge [C] = CURRENT_LSB x CHARGE, 40-bit two's complement. */
	fake_ina228_set_reg(REG_CHARGE, 320000u); /* 1.0 C */
	zassert_equal(ina228_read_charge_uc(&ctx, &c), ALP_OK);
	zassert_equal(c, 1000000);
	fake_ina228_set_reg(REG_CHARGE, 0xFFFFFFFFFFull); /* -1 count = -3.125 uC */
	zassert_equal(ina228_read_charge_uc(&ctx, &c), ALP_OK);
	zassert_equal(c, -3);
	fake_ina228_set_reg(REG_CHARGE, (0x10000000000ull - 320000ull) & 0xFFFFFFFFFFull); /* -1.0 C */
	zassert_equal(ina228_read_charge_uc(&ctx, &c), ALP_OK);
	zassert_equal(c, -1000000);
	fake_ina228_set_reg(REG_CHARGE, 0x8000000000ull); /* most negative: -2^39 counts */
	zassert_equal(ina228_read_charge_uc(&ctx, &c), ALP_OK);
	zassert_equal(c, -1717986918400ll);
	fake_ina228_set_reg(REG_CHARGE, 0x7FFFFFFFFFull); /* most positive */
	zassert_equal(ina228_read_charge_uc(&ctx, &c), ALP_OK);
	zassert_equal(c, 1717986918397ll);

	/* The scaling fails only when the RESULT does not fit 64 bits, never
	 * because the lsb x raw product is wider than 64 bits.  With all 40
	 * ENERGY bits set (1,099,511,627,775 counts) an LSB of 1e9 pA makes a
	 * 1.1e21 intermediate product, yet the result is a comfortable
	 * 56,294,995,342,080,000 uJ. */
	fake_ina228_set_reg(REG_ENERGY, 0xFFFFFFFFFFull);
	ctx.current_lsb_pa = 1000000000ull;
	zassert_equal(ina228_read_energy_uj(&ctx, &u), ALP_OK);
	zassert_equal(u, 56294995342080000ull);
	/* Exact boundary: 327,680,000,000 pA gives 18,446,744,073,692,774,400 uJ,
	 * the largest LSB whose result is <= 2^64 - 1; one pA more overflows
	 * (18,446,744,073,749,069,395 > 2^64 - 1) and is an error, not a wrap. */
	ctx.current_lsb_pa = 327680000000ull;
	zassert_equal(ina228_read_energy_uj(&ctx, &u), ALP_OK);
	zassert_equal(u, 18446744073692774400ull);
	ctx.current_lsb_pa = 327680000001ull;
	zassert_equal(ina228_read_energy_uj(&ctx, &u), ALP_ERR_OUT_OF_RANGE);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ina228_wire_format_is_big_endian)
{
	alp_i2c_t *bus = open_bus();
	ina228_t   ctx;

	fake_ina228_reset();
	init_ref(&ctx, bus, INA228_ADCRANGE_40MV);

	/* The fake stores a write [reg, hi, lo] as hi << 8 | lo.  A 40-bit read
	 * is answered most-significant-byte first, so a value with distinct
	 * bytes only decodes right if the driver assembles them in that order. */
	uint64_t u;
	fake_ina228_set_reg(REG_ENERGY, 0x0102030405ull);
	zassert_equal(ina228_read_energy_uj(&ctx, &u), ALP_OK);
	/* 0x0102030405 = 4328719365 counts; lsb 781250 pA: x 781250 x 256 / 5e6. */
	zassert_equal(u, (4328719365ull * 781250ull * 256ull + 2500000ull) / 5000000ull);

	/* SHUNT_CAL write: high byte first.  4096 = 0x1000 is symmetric-free of
	 * ambiguity only if hi != lo, so check an asymmetric one via the log. */
	zassert_true(fake_ina228_log_len() >= 2u);
	bool saw_cal = false;
	for (uint32_t i = 0; i < fake_ina228_log_len(); i++) {
		if (fake_ina228_log_reg(i) == REG_SHUNT_CAL) {
			zassert_equal(fake_ina228_log_val(i), 0x1000u);
			saw_cal = true;
		}
	}
	zassert_true(saw_cal);

	/* configure(): MODE | VBUSCT | VSHCT | VTCT | AVG in their fields. */
	zassert_equal(ina228_configure(&ctx,
	                               INA228_AVG_16,
	                               INA228_CT_84US,
	                               INA228_CT_150US,
	                               INA228_CT_280US,
	                               INA228_MODE_ALL_CONT),
	              ALP_OK);
	/* 0xF << 12 | 1 << 9 | 2 << 6 | 3 << 3 | 2 */
	zassert_equal(fake_ina228_get_reg(REG_ADC_CONFIG), 0xF29Au);
	zassert_equal(ina228_configure(&ctx,
	                               (ina228_avg_t)8,
	                               INA228_CT_84US,
	                               INA228_CT_150US,
	                               INA228_CT_280US,
	                               INA228_MODE_ALL_CONT),
	              ALP_ERR_INVAL);
	zassert_equal(ina228_configure(&ctx,
	                               INA228_AVG_1,
	                               INA228_CT_84US,
	                               INA228_CT_150US,
	                               INA228_CT_280US,
	                               (ina228_mode_t)16),
	              ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ina228_accumulator_reset_diag_and_reset)
{
	alp_i2c_t *bus = open_bus();
	ina228_t   ctx;
	uint16_t   diag;

	fake_ina228_reset();
	init_ref(&ctx, bus, INA228_ADCRANGE_40MV);

	/* RSTACC is set on top of the cached CONFIG (ADCRANGE kept), then the
	 * cached value is written back so the bit ends clear. */
	uint32_t before = fake_ina228_log_len();
	zassert_equal(ina228_reset_accumulators(&ctx), ALP_OK);
	zassert_equal(fake_ina228_log_len(), before + 2u);
	zassert_equal(fake_ina228_log_reg(before), REG_CONFIG);
	zassert_equal(fake_ina228_log_val(before), 0x4010u, "RSTACC | ADCRANGE");
	zassert_equal(fake_ina228_log_val(before + 1u), 0x0010u, "RSTACC cleared, ADCRANGE kept");

	/* DIAG_ALRT is returned raw. */
	fake_ina228_set_reg(REG_DIAG_ALRT,
	                    INA228_DIAG_CNVRF | INA228_DIAG_MEMSTAT | INA228_DIAG_MATHOF);
	zassert_equal(ina228_read_diag(&ctx, &diag), ALP_OK);
	zassert_equal(diag, 0x0203u);
	zassert_true(diag & INA228_DIAG_MATHOF);
	zassert_false(diag & INA228_DIAG_ENERGYOF);

	/* reset() writes RST, then re-applies ADCRANGE and SHUNT_CAL. */
	before = fake_ina228_log_len();
	zassert_equal(ina228_reset(&ctx), ALP_OK);
	zassert_equal(fake_ina228_log_reg(before), REG_CONFIG);
	zassert_equal(fake_ina228_log_val(before), 0x8000u);
	zassert_equal(fake_ina228_get_reg(REG_CONFIG), 0x0010u);
	zassert_equal(fake_ina228_get_reg(REG_SHUNT_CAL), 4096u);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ina228_uninitialised_context_is_rejected)
{
	ina228_t ctx_uninit = { 0 };
	int32_t  i;
	uint64_t u;
	int64_t  c;
	uint16_t d;

	zassert_equal(ina228_read_shunt_uv(NULL, &i), ALP_ERR_NOT_READY);
	zassert_equal(ina228_read_shunt_uv(&ctx_uninit, &i), ALP_ERR_NOT_READY);
	zassert_equal(ina228_read_bus_uv(&ctx_uninit, &i), ALP_ERR_NOT_READY);
	zassert_equal(ina228_read_temp_mdegc(&ctx_uninit, &i), ALP_ERR_NOT_READY);
	zassert_equal(ina228_read_current_ua(&ctx_uninit, &i), ALP_ERR_NOT_READY);
	zassert_equal(ina228_read_power_uw(&ctx_uninit, &u), ALP_ERR_NOT_READY);
	zassert_equal(ina228_read_energy_uj(&ctx_uninit, &u), ALP_ERR_NOT_READY);
	zassert_equal(ina228_read_charge_uc(&ctx_uninit, &c), ALP_ERR_NOT_READY);
	zassert_equal(ina228_read_diag(&ctx_uninit, &d), ALP_ERR_NOT_READY);
	zassert_equal(ina228_reset_accumulators(&ctx_uninit), ALP_ERR_NOT_READY);
	zassert_equal(ina228_reset(&ctx_uninit), ALP_ERR_NOT_READY);
	zassert_equal(ina228_configure(&ctx_uninit,
	                               INA228_AVG_1,
	                               INA228_CT_1052US,
	                               INA228_CT_1052US,
	                               INA228_CT_1052US,
	                               INA228_MODE_ALL_CONT),
	              ALP_ERR_NOT_READY);
	ina228_deinit(&ctx_uninit);
	ina228_deinit(NULL);

	/* A NULL output on an initialised context is rejected, not dereferenced. */
	fake_ina228_reset();
	alp_i2c_t *bus = open_bus();
	ina228_t   ctx;
	init_ref(&ctx, bus, INA228_ADCRANGE_163MV);
	zassert_equal(ina228_read_current_ua(&ctx, NULL), ALP_ERR_NOT_READY);
	ina228_deinit(&ctx);
	zassert_false(ctx.initialised);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ina228_both_ranges_report_the_same_engineering_values)
{
	alp_i2c_t *bus = open_bus();
	ina228_t   ctx;
	int32_t    i;
	uint64_t   u;

	/* 0.25 A through the 100 mOhm shunt = 25 mV across it, 5 V bus, 1.25 W. */
	fake_ina228_reset();
	init_ref(&ctx, bus, INA228_ADCRANGE_163MV);    /* 312.5 nV/LSB, 3.125 uA/LSB */
	fake_ina228_set_reg(REG_VSHUNT, 80000u << 4);  /* 25 mV / 312.5 nV */
	fake_ina228_set_reg(REG_CURRENT, 80000u << 4); /* 0.25 A / 3.125 uA */
	fake_ina228_set_reg(REG_POWER, 125000u);       /* 1.25 W / (3.2 x 3.125 uA) */
	zassert_equal(ina228_read_shunt_uv(&ctx, &i), ALP_OK);
	zassert_equal(i, 25000);
	zassert_equal(ina228_read_current_ua(&ctx, &i), ALP_OK);
	zassert_equal(i, 250000);
	zassert_equal(ina228_read_power_uw(&ctx, &u), ALP_OK);
	zassert_equal(u, 1250000u);

	fake_ina228_reset();
	init_ref(&ctx, bus, INA228_ADCRANGE_40MV);      /* 78.125 nV/LSB, 0.78125 uA/LSB */
	fake_ina228_set_reg(REG_VSHUNT, 320000u << 4);  /* 25 mV / 78.125 nV */
	fake_ina228_set_reg(REG_CURRENT, 320000u << 4); /* 0.25 A / 0.78125 uA */
	fake_ina228_set_reg(REG_POWER, 500000u);        /* 1.25 W / (3.2 x 0.78125 uA) */
	zassert_equal(ina228_read_shunt_uv(&ctx, &i), ALP_OK);
	zassert_equal(i, 25000);
	zassert_equal(ina228_read_current_ua(&ctx, &i), ALP_OK);
	zassert_equal(i, 250000);
	zassert_equal(ina228_read_power_uw(&ctx, &u), ALP_OK);
	zassert_equal(u, 1250000u);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ina228_set_shunt_range_rewrites_range_and_cal_and_resets_accumulators)
{
	alp_i2c_t *bus = open_bus();
	ina228_t   ctx;
	int32_t    i;

	fake_ina228_reset();
	init_ref(&ctx, bus, INA228_ADCRANGE_163MV);
	fake_ina228_set_reg(REG_ENERGY, 123456u);
	fake_ina228_set_reg(REG_CHARGE, 654321u);

	/* Wide -> narrow, reporting scale = the new full scale (0). */
	uint32_t before = fake_ina228_log_len();
	zassert_equal(ina228_set_shunt_range(&ctx, INA228_ADCRANGE_40MV, 0u), ALP_OK);
	zassert_equal(fake_ina228_log_len(), before + 3u, "three writes");
	/* 1: CONFIG = new ADCRANGE | RSTACC (accumulators cleared with the switch). */
	zassert_equal(fake_ina228_log_reg(before), REG_CONFIG);
	zassert_equal(fake_ina228_log_val(before), 0x4010u);
	/* 2: SHUNT_CAL for the new range (x4 for the range, /4 for the LSB). */
	zassert_equal(fake_ina228_log_reg(before + 1u), REG_SHUNT_CAL);
	zassert_equal(fake_ina228_log_val(before + 1u), 4096u);
	/* 3: CONFIG written back with RSTACC clear. */
	zassert_equal(fake_ina228_log_reg(before + 2u), REG_CONFIG);
	zassert_equal(fake_ina228_log_val(before + 2u), 0x0010u);
	zassert_equal(ctx.adcrange, INA228_ADCRANGE_40MV);
	zassert_equal(ctx.current_lsb_pa, 781250u);
	/* Scaling follows: the same CURRENT count now means a quarter the current. */
	fake_ina228_set_reg(REG_CURRENT, 320000u << 4);
	zassert_equal(ina228_read_current_ua(&ctx, &i), ALP_OK);
	zassert_equal(i, 250000);

	/* Narrow -> wide; an explicit scale below the full scale is honoured. */
	before = fake_ina228_log_len();
	zassert_equal(ina228_set_shunt_range(&ctx, INA228_ADCRANGE_163MV, 819200u), ALP_OK);
	zassert_equal(fake_ina228_log_val(before), 0x4000u, "ADCRANGE clear, RSTACC set");
	zassert_equal(fake_ina228_get_reg(REG_CONFIG), 0x0000u);
	zassert_equal(fake_ina228_get_reg(REG_SHUNT_CAL), 2048u, "half the 1.6384 A scale");

	/* Above the new range's full scale: rejected before the first write, and
	 * the context is untouched. */
	before = fake_ina228_log_len();
	zassert_equal(ina228_set_shunt_range(&ctx, INA228_ADCRANGE_40MV, 409601u),
	              ALP_ERR_OUT_OF_RANGE);
	zassert_equal(ina228_set_shunt_range(&ctx, (ina228_adcrange_t)2, 0u), ALP_ERR_INVAL);
	zassert_equal(fake_ina228_log_len(), before);
	zassert_equal(ctx.adcrange, INA228_ADCRANGE_163MV);

	/* Bus error part-way: reported unchanged, the context keeps its old range. */
	fake_ina228_set_error(-EBUSY);
	zassert_equal(ina228_set_shunt_range(&ctx, INA228_ADCRANGE_40MV, 0u), ALP_ERR_BUSY);
	fake_ina228_set_error(0);
	zassert_equal(ctx.adcrange, INA228_ADCRANGE_163MV);

	ina228_t ctx_uninit = { 0 };
	zassert_equal(ina228_set_shunt_range(&ctx_uninit, INA228_ADCRANGE_40MV, 0u), ALP_ERR_NOT_READY);
	zassert_equal(ina228_set_shunt_range(NULL, INA228_ADCRANGE_40MV, 0u), ALP_ERR_NOT_READY);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ina228_pick_adcrange_boundaries)
{
	ina228_adcrange_t r = INA228_ADCRANGE_163MV;

	/* 100 mOhm: narrow up to 0.4096 A inclusive, wide up to 1.6384 A inclusive. */
	zassert_equal(ina228_pick_adcrange(100000u, 1u, &r), ALP_OK);
	zassert_equal(r, INA228_ADCRANGE_40MV);
	zassert_equal(ina228_pick_adcrange(100000u, 409600u, &r), ALP_OK);
	zassert_equal(r, INA228_ADCRANGE_40MV);
	zassert_equal(ina228_pick_adcrange(100000u, 409601u, &r), ALP_OK);
	zassert_equal(r, INA228_ADCRANGE_163MV);
	zassert_equal(ina228_pick_adcrange(100000u, 1638400u, &r), ALP_OK);
	zassert_equal(r, INA228_ADCRANGE_163MV);
	/* Fits neither: an error, and the output is left alone. */
	r = INA228_ADCRANGE_40MV;
	zassert_equal(ina228_pick_adcrange(100000u, 1638401u, &r), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(r, INA228_ADCRANGE_40MV);
	/* Bad arguments. */
	zassert_equal(ina228_pick_adcrange(0u, 1000u, &r), ALP_ERR_INVAL);
	zassert_equal(ina228_pick_adcrange(100000u, 0u, &r), ALP_ERR_INVAL);
	zassert_equal(ina228_pick_adcrange(100000u, 1000u, NULL), ALP_ERR_INVAL);
	/* The same shunt-voltage boundary scales with R: 10 mOhm -> 4.096 A / 16.384 A. */
	zassert_equal(ina228_pick_adcrange(10000u, 4096000u, &r), ALP_OK);
	zassert_equal(r, INA228_ADCRANGE_40MV);
	zassert_equal(ina228_pick_adcrange(10000u, 4096001u, &r), ALP_OK);
	zassert_equal(r, INA228_ADCRANGE_163MV);
}

ZTEST(alp_chips, test_ina228_over_range_is_detected_not_reported_as_valid)
{
	alp_i2c_t *bus = open_bus();
	ina228_t   ctx;
	bool       over = true;

	fake_ina228_reset();
	init_ref(&ctx, bus, INA228_ADCRANGE_40MV);

	/* Mid-scale, no flag: valid. */
	fake_ina228_set_reg(REG_VSHUNT, 320000u << 4);
	zassert_equal(ina228_check_over_range(&ctx, &over), ALP_OK);
	zassert_false(over);
	/* The ADC holds its limit when clipped: both ends, and a few counts in. */
	fake_ina228_set_reg(REG_VSHUNT, 0x7FFFF0u);
	zassert_equal(ina228_check_over_range(&ctx, &over), ALP_OK);
	zassert_true(over);
	fake_ina228_set_reg(REG_VSHUNT, 0x800000u);
	zassert_equal(ina228_check_over_range(&ctx, &over), ALP_OK);
	zassert_true(over);
	fake_ina228_set_reg(REG_VSHUNT, (0x7FFFFu - INA228_SATURATION_COUNTS) << 4);
	zassert_equal(ina228_check_over_range(&ctx, &over), ALP_OK);
	zassert_true(over);
	fake_ina228_set_reg(REG_VSHUNT, (0x7FFFFu - INA228_SATURATION_COUNTS - 1u) << 4);
	zassert_equal(ina228_check_over_range(&ctx, &over), ALP_OK);
	zassert_false(over);
	/* MATHOF alone (current / power may be invalid) is over-range too. */
	fake_ina228_set_reg(REG_DIAG_ALRT, INA228_DIAG_MEMSTAT | INA228_DIAG_MATHOF);
	zassert_equal(ina228_check_over_range(&ctx, &over), ALP_OK);
	zassert_true(over);

	zassert_equal(ina228_check_over_range(&ctx, NULL), ALP_ERR_NOT_READY);
	ina228_t ctx_uninit = { 0 };
	zassert_equal(ina228_check_over_range(&ctx_uninit, &over), ALP_ERR_NOT_READY);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ina228_auto_range_policy_hysteresis)
{
	/* Wide range: drop to narrow only below 75 % of 40.96 mV (30720 uV). */
	zassert_false(range_policy_want_narrow(false, 38000, false), "stay wide at 38 mV");
	zassert_false(range_policy_want_narrow(false, 30720, false), "boundary: 30720 stays wide");
	zassert_true(range_policy_want_narrow(false, 30719, false), "just below -> narrow");
	zassert_true(range_policy_want_narrow(false, -30719, false), "sign is ignored");
	zassert_true(range_policy_want_narrow(false, 0, false));
	zassert_false(range_policy_want_narrow(false, 100, true), "a clipped reading never narrows");

	/* Narrow range: back to wide at 95 % (38912 uV) or when clipped. */
	zassert_true(range_policy_want_narrow(true, 38911, false), "just below -> stay narrow");
	zassert_false(range_policy_want_narrow(true, 38912, false), "boundary: 38912 goes wide");
	zassert_false(range_policy_want_narrow(true, -38912, false), "sign is ignored");
	zassert_false(range_policy_want_narrow(true, 100, true), "clipped -> wide at once");
	zassert_true(range_policy_want_narrow(true, 100, false));

	/* Inside the 30720..38912 band neither range is left: no flapping. */
	for (int32_t v = 30720; v < 38912; v += 512) {
		zassert_false(range_policy_want_narrow(false, v, false), "wide holds at %d", (int)v);
		zassert_true(range_policy_want_narrow(true, v, false), "narrow holds at %d", (int)v);
	}
}
