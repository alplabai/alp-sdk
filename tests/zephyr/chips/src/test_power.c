/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Power-tree chip smokes: act8760, da9292 (V2N primary/secondary PMICs),
 * pca9451a (E1M-NX9101 on-module PMIC), tps628640 (multi-instance buck).
 */

#include <zephyr/ztest.h>

#include "alp/chips/act8760.h"
#include "alp/chips/da9292.h"
#include "alp/chips/pca9451a.h"
#include "alp/chips/tps628640.h"
#include "alp/e1m_pinout.h"
#include "alp/peripheral.h"

/* ------------------------------------------------------------------ */
/* act8760 -- ACT88760 primary PMIC on V2N BRD_I2C                    */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_act8760_init_null_args)
{
	act8760_t  ctx;
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	zassert_equal(act8760_init(NULL, bus), ALP_ERR_INVAL);
	zassert_equal(act8760_init(&ctx, NULL), ALP_ERR_INVAL);

	/* _init_at takes the same constraints plus an explicit address. */
	zassert_equal(act8760_init_at(NULL, bus, ACT8760_I2C_ADDR_PAGE0), ALP_ERR_INVAL);
	zassert_equal(act8760_init_at(&ctx, NULL, ACT8760_I2C_ADDR_PAGE0), ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_act8760_calls_reject_uninitialised)
{
	act8760_t        ctx = { 0 };
	act8760_status_t status;
	uint8_t          v;

	zassert_equal(act8760_get_status(&ctx, &status), ALP_ERR_NOT_READY);
	zassert_equal(act8760_read_reg(&ctx, ACT8760_PAGE_SYSTEM, 0u, &v), ALP_ERR_NOT_READY);
	zassert_equal(act8760_write_reg(&ctx, ACT8760_PAGE_SYSTEM, 0u, 0u), ALP_ERR_NOT_READY);
	zassert_equal(act8760_rail_get_vset(&ctx, ACT8760_RAIL_BUCK1, &v), ALP_ERR_NOT_READY);
}

/* ------------------------------------------------------------------ */
/* da9292 -- DA9292 secondary PMIC on V2N BRD_I2C                     */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_da9292_init_null_args)
{
	da9292_t   ctx;
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	zassert_equal(da9292_init(NULL, bus, DA9292_I2C_ADDR_V2N), ALP_ERR_INVAL);
	zassert_equal(da9292_init(&ctx, NULL, DA9292_I2C_ADDR_V2N), ALP_ERR_INVAL);
	/* 8-bit-encoded address through the 7-bit API must be rejected. */
	zassert_equal(da9292_init(&ctx, bus, 0x80u), ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_da9292_calls_reject_uninitialised)
{
	da9292_t        ctx = { 0 };
	da9292_status_t status;
	da9292_events_t events;
	uint8_t         v;
	uint16_t        mv;

	zassert_equal(da9292_get_status(&ctx, &status), ALP_ERR_NOT_READY);
	zassert_equal(da9292_read_and_clear_events(&ctx, &events), ALP_ERR_NOT_READY);
	zassert_equal(da9292_set_enable(&ctx, DA9292_CH1, false), ALP_ERR_NOT_READY);
	zassert_equal(da9292_set_voltage_mv(&ctx, DA9292_CH1, 800u), ALP_ERR_NOT_READY);
	zassert_equal(da9292_get_voltage_mv(&ctx, DA9292_CH1, &mv), ALP_ERR_NOT_READY);
	zassert_equal(da9292_read_reg(&ctx, 0u, &v), ALP_ERR_NOT_READY);
	zassert_equal(da9292_v2n_base_init(&ctx), ALP_ERR_NOT_READY);
	zassert_equal(da9292_v2n_m1_enable_deepx_rail(&ctx, 5000u), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_da9292_set_voltage_range_validation)
{
	/* Force .initialised so the function reaches the range-check
     * before any bus access. */
	da9292_t ctx = { .initialised = true };

	/* Below the documented range (300..1275 mV). */
	zassert_equal(da9292_set_voltage_mv(&ctx, DA9292_CH1, 100u), ALP_ERR_INVAL);
	/* Above the documented range. */
	zassert_equal(da9292_set_voltage_mv(&ctx, DA9292_CH1, 2000u), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_da9292_get_fault_pins)
{
	uint8_t flags = 0xAAu;

	/* NULL out-pointer is the only hard-invalid argument. */
	zassert_equal(da9292_get_fault_pins(NULL, NULL, NULL), ALP_ERR_INVAL);

	/* Both pins NULL is legal (a board may wire neither): each bit
     * reports deasserted, so the packed byte must be 0x00 -- never
     * the bridge's 0xFF "no sample" sentinel. */
	zassert_equal(da9292_get_fault_pins(NULL, NULL, &flags), ALP_OK);
	zassert_equal(flags, 0x00u);
}

/* ------------------------------------------------------------------ */
/* pca9451a -- NXP PCA9451A on-module PMIC (E1M-NX9101)                */
/*                                                                     */
/* E1M-NX9101 doesn't exist on the bench yet (issue #474) -- these are */
/* host-side smoke tests against a real alp_i2c_t handle backed by the */
/* native_sim i2c-emul controller (no fake target attached), following */
/* the same lifecycle/NULL/range-validation pattern the act8760 and    */
/* da9292 PMIC drivers use.  Silicon validation is deferred to the     */
/* NX9101 HiL bring-up.                                                */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_pca9451a_init_null_args)
{
	pca9451a_t ctx;
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	zassert_equal(pca9451a_init(NULL, bus), ALP_ERR_INVAL, "NULL ctx must be invalid");
	zassert_equal(pca9451a_init(&ctx, NULL), ALP_ERR_INVAL, "NULL bus must be invalid");

	/* _init_at takes the same constraints plus an explicit address. */
	zassert_equal(
	    pca9451a_init_at(NULL, bus, PCA9451A_I2C_ADDR), ALP_ERR_INVAL, "NULL ctx must be invalid");
	zassert_equal(
	    pca9451a_init_at(&ctx, NULL, PCA9451A_I2C_ADDR), ALP_ERR_INVAL, "NULL bus must be invalid");
	zassert_equal(pca9451a_init_at(&ctx, bus, 0u), ALP_ERR_INVAL, "addr=0 must be invalid");

	alp_i2c_close(bus);
}

/* #739: OTP reprogramming has no fixed strap range this driver can
 * assert, so only the generic 7-bit domain bound applies (addr=0 is
 * NOT a fallback here -- see the addr=0 case above). */
ZTEST(alp_chips, test_pca9451a_init_at_validates_7bit_address_bound)
{
	pca9451a_t ctx;
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	zassert_not_equal(
	    pca9451a_init_at(&ctx, bus, 0x7Fu), ALP_ERR_INVAL, "0x7F is the last valid 7-bit address");
	zassert_equal(pca9451a_init_at(&ctx, bus, 0x80u), ALP_ERR_INVAL, "0x80 exceeds 7-bit domain");
	zassert_equal(pca9451a_init_at(&ctx, bus, 0xFFu), ALP_ERR_INVAL, "0xFF exceeds 7-bit domain");

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_pca9451a_post_init_calls_reject_uninitialised)
{
	/* Drives the real reg_read() path against the native_sim i2c-emul
     * controller (no fake PCA9451A target attached) -- exercises the
     * actual init() transfer, not just a NULL guard.  Without a real
     * chip behind the controller the probe is expected to fail, so
     * subsequent calls must report NOT_READY; if a future fake target
     * makes the ACK succeed, the branch below is skipped and the test
     * still passes (same conditional idiom as the icm42670/lsm6dso/
     * bme280 cases above). */
	pca9451a_t ctx;
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	alp_status_t s = pca9451a_init(&ctx, bus);
	if (s != ALP_OK) {
		pca9451a_status_t status;
		bool              enabled;
		int32_t           uv;
		uint8_t           v;
		zassert_equal(pca9451a_get_status(&ctx, &status), ALP_ERR_NOT_READY);
		zassert_equal(pca9451a_rail_set_enable(&ctx, PCA9451A_RAIL_BUCK1, true), ALP_ERR_NOT_READY);
		zassert_equal(pca9451a_rail_is_enabled(&ctx, PCA9451A_RAIL_BUCK1, &enabled),
		              ALP_ERR_NOT_READY);
		zassert_equal(pca9451a_rail_set_voltage_uv(&ctx, PCA9451A_RAIL_BUCK1, 800000),
		              ALP_ERR_NOT_READY);
		zassert_equal(pca9451a_rail_get_voltage_uv(&ctx, PCA9451A_RAIL_BUCK1, &uv),
		              ALP_ERR_NOT_READY);
		zassert_equal(pca9451a_read_reg(&ctx, PCA9451A_REG_DEV_ID, &v), ALP_ERR_NOT_READY);
		zassert_equal(pca9451a_write_reg(&ctx, PCA9451A_REG_DEV_ID, 0u), ALP_ERR_NOT_READY);
	}

	pca9451a_deinit(&ctx);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_pca9451a_rail_set_voltage_range_validation)
{
	/* Force .initialised so the range check is reached before any
     * bus access -- same idiom as test_da9292_set_voltage_range_validation. */
	pca9451a_t ctx = { .initialised = true };

	/* Below each rail kind's documented floor -> OUT_OF_RANGE. */
	zassert_equal(pca9451a_rail_set_voltage_uv(&ctx, PCA9451A_RAIL_BUCK1, 100000),
	              ALP_ERR_OUT_OF_RANGE,
	              "BUCK1 (DVS buck) floor is 600000 uV");
	zassert_equal(pca9451a_rail_set_voltage_uv(&ctx, PCA9451A_RAIL_BUCK4, 100000),
	              ALP_ERR_OUT_OF_RANGE,
	              "BUCK4 (std buck) floor is 600000 uV");
	zassert_equal(pca9451a_rail_set_voltage_uv(&ctx, PCA9451A_RAIL_LDO1, 100000),
	              ALP_ERR_OUT_OF_RANGE,
	              "LDO1 floor is 1600000 uV");
	zassert_equal(pca9451a_rail_set_voltage_uv(&ctx, PCA9451A_RAIL_LDO2, 100000),
	              ALP_ERR_OUT_OF_RANGE,
	              "LDO2 floor is 800000 uV");
	zassert_equal(pca9451a_rail_set_voltage_uv(&ctx, PCA9451A_RAIL_LDO5, 100000),
	              ALP_ERR_OUT_OF_RANGE,
	              "LDO5 floor is 1800000 uV");

	/* Out-of-range rail index -> INVAL, checked before any bus access. */
	zassert_equal(pca9451a_rail_set_voltage_uv(&ctx, (pca9451a_rail_t)PCA9451A_RAIL_COUNT, 1000000),
	              ALP_ERR_INVAL);
	bool enabled;
	zassert_equal(pca9451a_rail_is_enabled(&ctx, (pca9451a_rail_t)PCA9451A_RAIL_COUNT, &enabled),
	              ALP_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* tps628640 -- single-channel buck (multi-instance)                  */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_tps628640_init_null_args)
{
	tps628640_t ctx;
	alp_i2c_t  *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	zassert_equal(tps628640_init(NULL, bus, 0x44u, 1050u), ALP_ERR_INVAL);
	zassert_equal(tps628640_init(&ctx, NULL, 0x44u, 1050u), ALP_ERR_INVAL);
	/* 8-bit-encoded address through the 7-bit API. */
	zassert_equal(tps628640_init(&ctx, bus, 0x80u, 1050u), ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_tps628640_calls_reject_uninitialised)
{
	tps628640_t ctx = { 0 };
	uint16_t    mv;
	uint8_t     v;

	zassert_equal(tps628640_set_voltage_mv(&ctx, 1050u), ALP_ERR_NOT_READY);
	zassert_equal(tps628640_get_voltage_mv(&ctx, &mv), ALP_ERR_NOT_READY);
	zassert_equal(tps628640_get_status(&ctx, &v), ALP_ERR_NOT_READY);
	zassert_equal(tps628640_read_reg(&ctx, 0x01u, &v), ALP_ERR_NOT_READY);
	zassert_equal(tps628640_write_reg(&ctx, 0x01u, 0xA0u), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_tps628640_set_voltage_out_of_range)
{
	/* Force .initialised so the function reaches the range-check
     * before bus access.  Documented range 400..1675 mV. */
	tps628640_t ctx = { .initialised = true };
	zassert_equal(tps628640_set_voltage_mv(&ctx, 200u), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(tps628640_set_voltage_mv(&ctx, 2000u), ALP_ERR_OUT_OF_RANGE);
	/* Same range check applies to VOUT2 (the VID-pin-high register). */
	zassert_equal(tps628640_set_voltage2_mv(&ctx, 200u), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(tps628640_set_voltage2_mv(&ctx, 2000u), ALP_ERR_OUT_OF_RANGE);
}

ZTEST(alp_chips, test_tps628640_control_helpers_reject_uninitialised)
{
	/* Datasheet-integration follow-up: typed CONTROL helpers must
     * report NOT_READY on a zeroed context like every other call. */
	tps628640_t ctx = { 0 };
	zassert_equal(tps628640_software_enable(&ctx, true), ALP_ERR_NOT_READY);
	zassert_equal(tps628640_set_fpwm_mode(&ctx, true), ALP_ERR_NOT_READY);
	zassert_equal(tps628640_set_ramp_speed(&ctx, TPS628640_RAMP_1_MV_PER_US), ALP_ERR_NOT_READY);
	zassert_equal(tps628640_reset_to_defaults(&ctx), ALP_ERR_NOT_READY);
	uint16_t mv;
	zassert_equal(tps628640_get_voltage2_mv(&ctx, &mv), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_tps628640_set_ramp_speed_invalid)
{
	/* Force .initialised so the function reaches the enum check.
     * Documented range 0..3; 4 is invalid. */
	tps628640_t ctx = { .initialised = true };
	zassert_equal(tps628640_set_ramp_speed(&ctx, (tps628640_ramp_speed_t)4u), ALP_ERR_INVAL);
}
