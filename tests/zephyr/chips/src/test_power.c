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
#include "alp/chips/v2n_power_tree.h"
#include "alp/e1m_pinout.h"
#include "alp/peripheral.h"

#include <zephyr/drivers/gpio/gpio_emul.h>

#include "fakes.h"

/* Not part of the public da9292 API -- see chips/da9292/da9292_internal.h
 * for why the poll-budget decision is tested directly here rather than
 * through da9292_ch2_sequence(). */
#include "da9292_internal.h"

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

	act8760_rail_state_t rs;
	act8760_gpio_state_t gs;
	uint16_t             mv, mask;
	zassert_equal(act8760_set_limits(&ctx, NULL, 0u), ALP_ERR_NOT_READY);
	zassert_equal(act8760_rail_get_state(&ctx, ACT8760_RAIL_BUCK1, &rs), ALP_ERR_NOT_READY);
	zassert_equal(act8760_rail_get_voltage_mv(&ctx, ACT8760_RAIL_BUCK1, &mv), ALP_ERR_NOT_READY);
	zassert_equal(act8760_rail_set_voltage_mv(&ctx, ACT8760_RAIL_BUCK1, 3300u), ALP_ERR_NOT_READY);
	zassert_equal(act8760_rail_set_enable(&ctx, ACT8760_RAIL_BUCK1, true), ALP_ERR_NOT_READY);
	zassert_equal(act8760_gpio_get(&ctx, 4u, &gs), ALP_ERR_NOT_READY);
	zassert_equal(act8760_gpio_toggles_peek(&ctx, &mask), ALP_ERR_NOT_READY);
	zassert_equal(act8760_gpio_toggles_clear(&ctx, &mask), ALP_ERR_NOT_READY);
	zassert_equal(act8760_gpio_set_polarity(&ctx, 4u, false), ALP_ERR_NOT_READY);
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
	zassert_equal(da9292_write_reg(&ctx, 0x02u, 0u), ALP_ERR_NOT_READY);
	zassert_equal(da9292_set_limits(&ctx, NULL), ALP_ERR_NOT_READY);
	zassert_equal(da9292_peek_events(&ctx, &events), ALP_ERR_NOT_READY);

	da9292_identity_t      id;
	da9292_channel_state_t cs;
	zassert_equal(da9292_get_identity(&ctx, &id), ALP_ERR_NOT_READY);
	zassert_equal(da9292_get_channel_state(&ctx, DA9292_CH2, &cs), ALP_ERR_NOT_READY);
	zassert_equal(da9292_ch2_sequence(&ctx, NULL, NULL), ALP_ERR_NOT_READY);
}

/* Fail-closed: with no limits table installed every control write is
 * refused BEFORE any bus access (.initialised forced, no bus). */
ZTEST(alp_chips, test_da9292_control_writes_fail_closed)
{
	da9292_t                        ctx = { .initialised = true };
	struct da9292_ch2_seq_result    res;
	const struct da9292_ch2_seq_cfg cfg = {
		.target_mv       = 750u,
		.expected_dev_id = 0xEAu,
		.pwr_en_req      = (alp_gpio_t *)&ctx, /* never dereferenced */
		.core_en         = (alp_gpio_t *)&ctx,
		.delay           = NULL,
	};

	zassert_equal(da9292_set_voltage_mv(&ctx, DA9292_CH2, 750u), ALP_ERR_NOSUPPORT);
	zassert_equal(da9292_set_enable(&ctx, DA9292_CH2, true), ALP_ERR_NOSUPPORT);
	zassert_equal(da9292_set_enable(&ctx, DA9292_CH1, false), ALP_ERR_NOSUPPORT);
	zassert_equal(da9292_write_reg(&ctx, 0x02u, 0xFFu), ALP_ERR_NOSUPPORT);
	/* Missing delay callback is an argument error before the guard. */
	zassert_equal(da9292_ch2_sequence(&ctx, &cfg, &res), ALP_ERR_INVAL);
	zassert_equal(res.step, DA9292_SEQ_ERR_ARGS);
	/* Invalid channel is rejected before the guard. */
	zassert_equal(da9292_set_voltage_mv(&ctx, (da9292_channel_t)2, 750u), ALP_ERR_INVAL);
}

/* #757 regression: da9292_ch2_sequence()'s DEEPX_PWR_EN_REQ / CH2_PG poll loops
 * must actually time out for a bounded budget, and never infinite-loop
 * (or wrongly succeed) at timeout_us == UINT32_MAX -- the pathological
 * "wait forever" caller value.  The I2C test double can't drive the
 * end-to-end path (see the include comment above), so this exercises
 * the exact boundary function the loop calls every pass. */
ZTEST(alp_chips, test_da9292_poll_budget_step_boundaries)
{
	/* Budget exactly covers one more slice: this IS still a valid pass
	 * (poll_us fits, so keep polling once more), landing *remaining_us
	 * at exactly 0. */
	uint32_t remaining_us = 100u;
	zassert_true(da9292_poll_budget_step(&remaining_us, 100u),
	             "remaining_us == poll_us must still cover one more pass");
	zassert_equal(remaining_us, 0u, "must decrement to exactly 0, not underflow");

	/* *Next* pass has nothing left -- must report "stop" (timeout)
	 * without touching *remaining_us. */
	zassert_false(da9292_poll_budget_step(&remaining_us, 100u), "remaining_us == 0 must time out");
	zassert_equal(remaining_us, 0u, "timeout return must not mutate *remaining_us");

	/* One tick short of a full slice: also times out immediately. */
	remaining_us = 99u;
	zassert_false(da9292_poll_budget_step(&remaining_us, 100u));

	/* Comfortably above one slice: keeps polling and decrements. */
	remaining_us = 250u;
	zassert_true(da9292_poll_budget_step(&remaining_us, 100u),
	             "must keep polling with budget left");
	zassert_equal(remaining_us, 150u);
	zassert_true(da9292_poll_budget_step(&remaining_us, 100u));
	zassert_equal(remaining_us, 50u);
	zassert_false(da9292_poll_budget_step(&remaining_us, 100u),
	              "final slice must time out, not underflow");
	zassert_equal(remaining_us, 50u, "still not mutated on the timing-out call");

	/* #757's named pathological input: timeout_us == UINT32_MAX must
	 * still strictly decrease every pass (no wrap, no infinite loop)
	 * and eventually reach the timeout branch in a bounded number of
	 * iterations -- it must never silently report "keep waiting"
	 * forever nor claim success. Walk enough iterations to prove the
	 * budget is monotonically decreasing, not that it's fully spent
	 * (that would take ~43M iterations at poll_us=100). */
	remaining_us     = UINT32_MAX;
	uint32_t poll_us = 100u;
	uint32_t prev    = remaining_us;
	for (int i = 0; i < 1000; i++) {
		bool keep_polling = da9292_poll_budget_step(&remaining_us, poll_us);
		zassert_true(keep_polling, "UINT32_MAX budget must not time out this early");
		zassert_true(remaining_us < prev, "budget must strictly decrease every pass (no wrap)");
		prev = remaining_us;
	}
	zassert_equal(remaining_us, UINT32_MAX - 1000u * poll_us);
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
     * controller at 0x2A, where no fake target sits (the default 0x25
     * is the fake ACT88760) -- exercises the actual init() transfer,
     * not just a NULL guard.  The probe must fail, so subsequent calls
     * must report NOT_READY. */
	pca9451a_t ctx;
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	zassert_not_equal(pca9451a_init_at(&ctx, bus, 0x2Au), ALP_OK, "no target at 0x2A");
	{
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
	zassert_equal(tps628640_set_limits(&ctx, NULL), ALP_ERR_NOT_READY);
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

/* ------------------------------------------------------------------ */
/* PMIC guard + sequence coverage against the fake_act8760 /           */
/* fake_da9292 / fake_tps628640 i2c-emul targets.  Every expectation   */
/* is an exact register byte: these paths can drop a live rail.        */
/* ------------------------------------------------------------------ */

static alp_i2c_t *pmic_bus_open(void)
{
	return alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
}

/* ---- ACT88760 ------------------------------------------------------ */

static const pmic_rail_limit_t act_v2n_limits[ACT8760_RAIL_COUNT] =
    V2N_POWER_ACT8760_RAIL_LIMITS_INIT;

/* Buck1 tile on ADD1 (0x40): status 0x40, VSET0 0x42, ON 0x44, range
 * 0x46 bit1.  VSET0 bit7 is a foreign field the driver must preserve. */
static alp_i2c_t *act_setup(act8760_t *ctx)
{
	fake_act8760_reset();
	fake_act8760_set_reg(0u, 0x40u, 0x80u); /* POK */
	fake_act8760_set_reg(0u, 0x42u, 0xF0u); /* bit7 + VSET 0x70 = 3300 mV at range 1 */
	fake_act8760_set_reg(0u, 0x44u, 0x80u); /* ON */
	fake_act8760_set_reg(0u, 0x46u, 0x02u); /* Vout_Range = 1 (25 mV step) */
	fake_act8760_set_reg(0u, 0x10u, 0x88u); /* MODE4: GPIO4 OTP defect, inverted */
	fake_act8760_set_reg(0u, 0x11u, 0x05u); /* MODE5 */

	alp_i2c_t *bus = pmic_bus_open();
	zassert_not_null(bus);
	zassert_ok(act8760_init(ctx, bus));
	return bus;
}

ZTEST(alp_chips, test_act8760_write_reg_deny_and_allow)
{
	act8760_t  ctx;
	alp_i2c_t *bus = act_setup(&ctx);

	/* No table: even an allow-listed address is refused. */
	zassert_equal(act8760_write_reg(&ctx, ACT8760_PAGE_SYSTEM, 0x05u, 0xFFu), ALP_ERR_NOSUPPORT);
	zassert_ok(
	    act8760_set_limits(&ctx, act_v2n_limits, V2N_POWER_ACT8760_GPIO_POLARITY_WRITABLE_MASK));

	/* Hard-deny: MSTR 0x07 (MR / SLEEP / DPSLP / POWER OFF / watchdog),
	 * 0x09, 0x0A, the IO-delay / WDTIME registers 0x0B / 0x0C, 0x14
	 * (POK_OV / VSYSWARN thresholds -- a bad value can trip PMIC
	 * shutdown), factory 0x15..0x26 and 0x2D..0x32. */
	static const uint8_t deny[] = { 0x07u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu, 0x14u };
	for (size_t i = 0; i < ARRAY_SIZE(deny); i++) {
		zassert_equal(act8760_write_reg(&ctx, ACT8760_PAGE_SYSTEM, deny[i], 0x00u),
		              ALP_ERR_NOSUPPORT,
		              "0x%02x must be denied",
		              deny[i]);
	}
	for (unsigned r = 0x15u; r <= 0x26u; r++) {
		zassert_equal(act8760_write_reg(&ctx, ACT8760_PAGE_SYSTEM, (uint8_t)r, 0x00u),
		              ALP_ERR_NOSUPPORT,
		              "0x%02x must be denied",
		              r);
	}
	for (unsigned r = 0x2Du; r <= 0x32u; r++) {
		zassert_equal(act8760_write_reg(&ctx, ACT8760_PAGE_SYSTEM, (uint8_t)r, 0x00u),
		              ALP_ERR_NOSUPPORT,
		              "0x%02x must be denied",
		              r);
	}
	/* ADD2 (Buck7 + LDOs) is never raw-writable. */
	zassert_equal(act8760_write_reg(&ctx, ACT8760_PAGE_AUX, 0x05u, 0x00u), ALP_ERR_NOSUPPORT);
	zassert_equal(fake_act8760_log_len(), 0u, "a refused write must never reach the bus");

	/* The allow-list lands, one byte each. */
	static const uint8_t allow[] = { 0x01u, 0x05u, 0x2Bu, 0x33u };
	for (size_t i = 0; i < ARRAY_SIZE(allow); i++) {
		const uint8_t val = (uint8_t)(0x5Au + i);
		zassert_ok(act8760_write_reg(&ctx, ACT8760_PAGE_SYSTEM, allow[i], val));
		zassert_equal(fake_act8760_get_reg(0u, allow[i]), val);
		zassert_equal(fake_act8760_write_count(0u, allow[i]), 1u);
	}
	zassert_equal(fake_act8760_log_len(), ARRAY_SIZE(allow));

	act8760_deinit(&ctx);
	alp_i2c_close(bus);
}

/* Buck3 (LPD4x_1V1): Vout_Range is tile +1 bit3 (0x81), not +6 (DBSTBY). */
ZTEST(alp_chips, test_act8760_buck3_range_at_tile_plus1_bit3)
{
	act8760_t  ctx;
	alp_i2c_t *bus = act_setup(&ctx);
	uint16_t   mv  = 0;

	fake_act8760_set_reg(0u, 0x82u, 0x18u); /* VSET 24 */
	fake_act8760_set_reg(0u, 0x86u, 0x03u); /* DBSTBY -- must not read as range */
	fake_act8760_set_reg(0u, 0x81u, 0x00u); /* range 0: 500 + 24 x 5 = 620 mV */
	zassert_ok(act8760_rail_get_voltage_mv(&ctx, ACT8760_RAIL_BUCK3, &mv));
	zassert_equal(mv, 620u);
	fake_act8760_set_reg(0u, 0x81u, 0x08u); /* range 1: 500 + 24 x 25 = 1100 mV */
	zassert_ok(act8760_rail_get_voltage_mv(&ctx, ACT8760_RAIL_BUCK3, &mv));
	zassert_equal(mv, 1100u);

	zassert_ok(
	    act8760_set_limits(&ctx, act_v2n_limits, V2N_POWER_ACT8760_GPIO_POLARITY_WRITABLE_MASK));
	zassert_equal(act8760_rail_set_voltage_mv(&ctx, ACT8760_RAIL_BUCK3, 1175u),
	              ALP_ERR_OUT_OF_RANGE);
	zassert_ok(act8760_rail_set_voltage_mv(&ctx, ACT8760_RAIL_BUCK3, 1125u));
	zassert_equal(fake_act8760_get_reg(0u, 0x82u), 0x19u);
	zassert_equal(fake_act8760_write_count(0u, 0x81u), 0u, "range bit never written");
	zassert_equal(fake_act8760_write_count(0u, 0x86u), 0u);

	act8760_deinit(&ctx);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_act8760_gpio_polarity_bit7_only)
{
	act8760_t  ctx;
	alp_i2c_t *bus = act_setup(&ctx);

	zassert_equal(act8760_gpio_set_polarity(&ctx, 4u, false), ALP_ERR_NOSUPPORT, "no table");
	zassert_equal(fake_act8760_log_len(), 0u);
	zassert_ok(
	    act8760_set_limits(&ctx, act_v2n_limits, V2N_POWER_ACT8760_GPIO_POLARITY_WRITABLE_MASK));

	/* GPIO4 GD32_NRST: MODE4 0x88 -> 0x08, bit7 only, MUX 0x08 kept. */
	zassert_ok(act8760_gpio_set_polarity(&ctx, 4u, false));
	zassert_equal(fake_act8760_get_reg(0u, 0x10u), V2N_POWER_ACT8760_GPIO4_EXPECTED_MODE);
	zassert_equal(fake_act8760_log_len(), 1u, "exactly one write: MODE4");
	zassert_equal(fake_act8760_log(0)->page, 0u);
	zassert_equal(fake_act8760_log(0)->reg, 0x10u);
	zassert_equal(fake_act8760_log(0)->val, 0x08u);

	/* Already there: no second write. */
	zassert_ok(act8760_gpio_set_polarity(&ctx, 4u, false));
	zassert_equal(fake_act8760_log_len(), 1u);

	/* GPIO5 V2N_BOOT_CPU_SEL is outside the writable mask. */
	zassert_equal(act8760_gpio_set_polarity(&ctx, 5u, true), ALP_ERR_NOSUPPORT);
	zassert_equal(fake_act8760_get_reg(0u, 0x11u), 0x05u);
	zassert_equal(fake_act8760_log_len(), 1u);

	act8760_gpio_state_t gs;
	zassert_ok(act8760_gpio_get(&ctx, 4u, &gs));
	zassert_false(gs.inverted);
	zassert_equal(gs.mux, 0x08u);
	zassert_equal(gs.mode_raw, 0x08u);

	act8760_deinit(&ctx);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_act8760_rail_writes_are_limit_gated)
{
	act8760_t  ctx;
	alp_i2c_t *bus = act_setup(&ctx);
	uint16_t   mv  = 0;

	zassert_ok(act8760_rail_get_voltage_mv(&ctx, ACT8760_RAIL_BUCK1, &mv));
	zassert_equal(mv, 3300u);

	/* No table: refused, nothing on the bus. */
	zassert_equal(act8760_rail_set_voltage_mv(&ctx, ACT8760_RAIL_BUCK1, 3400u), ALP_ERR_NOSUPPORT);
	zassert_equal(act8760_rail_set_enable(&ctx, ACT8760_RAIL_BUCK1, true), ALP_ERR_NOSUPPORT);
	zassert_equal(fake_act8760_log_len(), 0u);

	zassert_ok(
	    act8760_set_limits(&ctx, act_v2n_limits, V2N_POWER_ACT8760_GPIO_POLARITY_WRITABLE_MASK));

	/* VDD_3V3 window [3150, 3450]. */
	zassert_equal(act8760_rail_set_voltage_mv(&ctx, ACT8760_RAIL_BUCK1, 3500u),
	              ALP_ERR_OUT_OF_RANGE);
	zassert_equal(act8760_rail_set_voltage_mv(&ctx, ACT8760_RAIL_BUCK1, 3100u),
	              ALP_ERR_OUT_OF_RANGE);
	zassert_equal(fake_act8760_log_len(), 0u);

	zassert_ok(act8760_rail_set_voltage_mv(&ctx, ACT8760_RAIL_BUCK1, 3400u));
	zassert_equal(fake_act8760_get_reg(0u, 0x42u), 0xF4u, "VSET 0x74, bit7 preserved");
	zassert_equal(fake_act8760_get_reg(0u, 0x46u), 0x02u, "range bit never flipped");
	zassert_equal(fake_act8760_write_count(0u, 0x46u), 0u);

	/* Critical rail: disable refused, ON bit untouched. */
	zassert_equal(act8760_rail_set_enable(&ctx, ACT8760_RAIL_BUCK1, false), ALP_ERR_NOSUPPORT);
	zassert_equal(fake_act8760_get_reg(0u, 0x44u), 0x80u);
	zassert_equal(fake_act8760_write_count(0u, 0x44u), 0u);

	act8760_deinit(&ctx);
	alp_i2c_close(bus);
}

/* A non-critical enable-only entry disables, and the write lands on ADD2
 * (LDO5 tile slot 0x40: ON at 0x42) -- not on the same offset of ADD1. */
ZTEST(alp_chips, test_act8760_noncritical_disable_hits_add2)
{
	act8760_t  ctx;
	alp_i2c_t *bus = act_setup(&ctx);
	fake_act8760_set_reg(1u, 0x42u, 0x80u);

	static const pmic_rail_limit_t only_ldo5[ACT8760_RAIL_COUNT] = {
		[ACT8760_RAIL_LDO5] = { .enable_writable = true },
	};
	zassert_ok(act8760_set_limits(&ctx, only_ldo5, 0u));

	zassert_equal(act8760_rail_set_enable(&ctx, ACT8760_RAIL_BUCK1, true),
	              ALP_ERR_NOSUPPORT,
	              "an all-zero entry grants no control");
	zassert_ok(act8760_rail_set_enable(&ctx, ACT8760_RAIL_LDO5, false));
	zassert_equal(fake_act8760_get_reg(1u, 0x42u), 0x00u);
	zassert_equal(fake_act8760_get_reg(0u, 0x42u), 0xF0u, "ADD1 0x42 untouched");
	zassert_equal(fake_act8760_log_len(), 1u);
	zassert_equal(fake_act8760_log(0)->page, 1u);

	act8760_deinit(&ctx);
	alp_i2c_close(bus);
}

/* act8760_rail_set_enable(true) must refuse a live VSET the guard window no
 * longer covers, the same rule da9292_set_enable() / software_enable()
 * already apply -- exercised through a synthetic voltage+enable-writable
 * entry, since no real V2N rail combines both (every ACT88760 "voltage"
 * rail in the power tree is CMI-hardware-sequenced and enable_writable is
 * false there by design; see power-tree.yaml's control-class comment). */
ZTEST(alp_chips, test_act8760_rail_set_enable_refuses_vout_outside_window)
{
	act8760_t  ctx;
	alp_i2c_t *bus = act_setup(&ctx);

	static const pmic_rail_limit_t buck1_ve[ACT8760_RAIL_COUNT] = {
		[ACT8760_RAIL_BUCK1] = { .min_mv           = 3150u,
		                         .max_mv           = 3450u,
		                         .voltage_writable = true,
		                         .enable_writable  = true },
	};
	zassert_ok(act8760_set_limits(&ctx, buck1_ve, 0u));

	/* Bench-programmed VSET0 (0xF0, range 1) decodes to 3300 mV, inside
	 * [3150, 3450]: enabling succeeds. */
	zassert_ok(act8760_rail_set_enable(&ctx, ACT8760_RAIL_BUCK1, true));
	zassert_equal(fake_act8760_get_reg(0u, 0x44u), 0x80u);

	/* Move VSET outside the window via a direct fake write (as if a
	 * stale/POR value never went through the guarded setter) and confirm
	 * a fresh enable now refuses without ever touching ON. */
	fake_act8760_set_reg(0u, 0x44u, 0x00u); /* start from OFF to observe the refusal */
	fake_act8760_set_reg(0u, 0x42u, 0x00u); /* VSET 0 -> 500 mV, outside [3150, 3450] */
	zassert_equal(act8760_rail_set_enable(&ctx, ACT8760_RAIL_BUCK1, true), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(fake_act8760_get_reg(0u, 0x44u), 0x00u, "refused enable must never touch ON");
	zassert_equal(fake_act8760_write_count(0u, 0x44u), 0u);

	act8760_deinit(&ctx);
	alp_i2c_close(bus);
}

/* ---- TPS628640 ----------------------------------------------------- */

static const pmic_rail_limit_t tps_l44  = V2N_M1_POWER_TPS628640_DDR5_VDD2H_1V05_LIMIT_INIT;
static const pmic_rail_limit_t tps_l48  = V2N_M1_POWER_TPS628640_VDD0V85_LPDDR_LIMIT_INIT;
static const pmic_rail_limit_t tps_l4f  = V2N_M1_POWER_TPS628640_DDR5_VDDQ_0V5_LIMIT_INIT;
static const pmic_rail_limit_t tps_crit = V2N_M1_POWER_TPS628640_LPD4X_0V6_LIMIT_INIT;

static alp_i2c_t *tps_setup(tps628640_t *b44, tps628640_t *b48, tps628640_t *b4f)
{
	fake_tps628640_reset(0x44u);
	fake_tps628640_reset(0x48u);
	fake_tps628640_reset(0x4Fu);
	alp_i2c_t *bus = pmic_bus_open();
	zassert_not_null(bus);
	zassert_ok(tps628640_init(b44, bus, 0x44u, 1050u));
	zassert_ok(tps628640_init(b48, bus, 0x48u, 850u));
	zassert_ok(tps628640_init(b4f, bus, 0x4Fu, 500u));
	return bus;
}

ZTEST(alp_chips, test_tps628640_per_instance_windows)
{
	tps628640_t b44, b48, b4f;
	alp_i2c_t  *bus = tps_setup(&b44, &b48, &b4f);

	/* No entry: every control write refused, bus untouched. */
	zassert_equal(tps628640_set_voltage_mv(&b48, 850u), ALP_ERR_NOSUPPORT);
	zassert_equal(tps628640_software_enable(&b4f, false), ALP_ERR_NOSUPPORT);
	zassert_equal(tps628640_set_fpwm_mode(&b4f, true), ALP_ERR_NOSUPPORT);
	zassert_equal(tps628640_set_ramp_speed(&b4f, TPS628640_RAMP_1_MV_PER_US), ALP_ERR_NOSUPPORT);
	zassert_equal(tps628640_reset_to_defaults(&b4f), ALP_ERR_NOSUPPORT);
	zassert_equal(fake_tps628640_write_count(0x48u, TPS628640_REG_VOUT1), 0u);
	zassert_equal(fake_tps628640_write_count(0x4Fu, TPS628640_REG_CONTROL), 0u);

	zassert_ok(tps628640_set_limits(&b44, &tps_l44));
	zassert_ok(tps628640_set_limits(&b48, &tps_l48));
	zassert_ok(tps628640_set_limits(&b4f, &tps_l4f));

	/* 850 mV is inside 0x48's [810, 890] but not 0x44's [1000, 1100]. */
	zassert_equal(tps628640_set_voltage_mv(&b44, 850u), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(fake_tps628640_write_count(0x44u, TPS628640_REG_VOUT1), 0u);
	zassert_ok(tps628640_set_voltage_mv(&b48, 850u));
	zassert_equal(fake_tps628640_get_reg(0x48u, TPS628640_REG_VOUT1), 0x5Au);
	zassert_ok(tps628640_set_voltage_mv(&b44, 1050u));
	zassert_equal(fake_tps628640_get_reg(0x44u, TPS628640_REG_VOUT1), 0x82u);

	/* 0x4F [475, 525]: 530 refused; 524 rounds down to 0x18 = 520 mV. */
	zassert_equal(tps628640_set_voltage_mv(&b4f, 530u), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(tps628640_set_voltage2_mv(&b4f, 470u), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(fake_tps628640_write_count(0x4Fu, TPS628640_REG_VOUT1), 0u);
	zassert_ok(tps628640_set_voltage_mv(&b4f, 524u));
	zassert_equal(fake_tps628640_get_reg(0x4Fu, TPS628640_REG_VOUT1), 0x18u);

	tps628640_deinit(&b44);
	tps628640_deinit(&b48);
	tps628640_deinit(&b4f);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_tps628640_software_enable_gating)
{
	tps628640_t b44, b48, b4f;
	alp_i2c_t  *bus = tps_setup(&b44, &b48, &b4f);

	/* Non-critical DDR5_VDDQ_0V5: off then on, SOFTWARE_ENABLE only. */
	zassert_ok(tps628640_set_limits(&b4f, &tps_l4f));
	zassert_ok(tps628640_software_enable(&b4f, false));
	zassert_equal(fake_tps628640_get_reg(0x4Fu, TPS628640_REG_CONTROL), 0x4Fu);
	zassert_ok(tps628640_software_enable(&b4f, true));
	zassert_equal(fake_tps628640_get_reg(0x4Fu, TPS628640_REG_CONTROL), 0x6Fu);

	/* A critical entry (LPD4x_0V6's) is never switched off, and a reset
	 * counts as a switch-off. */
	zassert_ok(tps628640_set_limits(&b44, &tps_crit));
	zassert_equal(tps628640_software_enable(&b44, false), ALP_ERR_NOSUPPORT);
	zassert_equal(tps628640_reset_to_defaults(&b44), ALP_ERR_NOSUPPORT);
	zassert_equal(fake_tps628640_write_count(0x44u, TPS628640_REG_CONTROL), 0u);
	zassert_equal(fake_tps628640_get_reg(0x44u, TPS628640_REG_CONTROL), 0x6Fu);
	/* b44 (addr 0x44) is on-the-bench at 1050 mV, outside tps_crit's
	 * [570, 630] LPD4x_0V6 window borrowed here for the critical-flag
	 * check -- program a live setpoint inside that window first, since
	 * enabling now validates VOUT1 *and* VOUT2 against the installed
	 * window (the driver can't read the VID strap that picks which one
	 * is live, so both must be confirmed). */
	zassert_ok(tps628640_set_voltage_mv(&b44, 600u));
	zassert_ok(tps628640_set_voltage2_mv(&b44, 600u));
	zassert_ok(tps628640_software_enable(&b44, true));

	tps628640_deinit(&b44);
	tps628640_deinit(&b48);
	tps628640_deinit(&b4f);
	alp_i2c_close(bus);
}

/* Enabling refuses a live VOUT1 outside the installed window instead of
 * blindly energizing it -- exercised on 0x4F/DDR5_VDDQ_0V5, whose VOUT1
 * (0x14 = 500 mV) is always present once the chip answers at all, so there
 * is no "not populated yet" excuse for the check to skip it. */
ZTEST(alp_chips, test_tps628640_enable_refuses_vout_outside_window)
{
	tps628640_t b44, b48, b4f;
	alp_i2c_t  *bus = tps_setup(&b44, &b48, &b4f);

	zassert_ok(tps628640_set_limits(&b4f, &tps_l4f));

	/* POR VOUT1 (0x14 = 500 mV) is inside [475, 525]: enabling succeeds. */
	zassert_ok(tps628640_software_enable(&b4f, true));
	zassert_ok(tps628640_software_enable(&b4f, false));

	/* Move VOUT1 out of the window via a direct fake write (as if a
	 * stale/POR value never went through the guarded setter) and confirm
	 * enabling now refuses without ever touching CONTROL. */
	fake_tps628640_set_reg(0x4Fu, TPS628640_REG_VOUT1, 0x00u /* 400 mV */);
	zassert_equal(tps628640_software_enable(&b4f, true), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(
	    fake_tps628640_write_count(0x4Fu, TPS628640_REG_CONTROL),
	    2u,
	    "the earlier enable+disable wrote CONTROL twice; the refused re-enable adds none");

	tps628640_deinit(&b44);
	tps628640_deinit(&b48);
	tps628640_deinit(&b4f);
	alp_i2c_close(bus);
}

/* The VID pin -- not the driver -- picks whether VOUT1 or VOUT2 is live, and
 * the driver cannot read that strap, so enabling must refuse when EITHER
 * setpoint is outside the window, not just VOUT1. */
ZTEST(alp_chips, test_tps628640_enable_refuses_vout2_outside_window)
{
	tps628640_t b44, b48, b4f;
	alp_i2c_t  *bus = tps_setup(&b44, &b48, &b4f);

	zassert_ok(tps628640_set_limits(&b4f, &tps_l4f));

	/* VOUT1 stays at its in-window POR value; only VOUT2 moves outside
	 * [475, 525]. */
	fake_tps628640_set_reg(0x4Fu, TPS628640_REG_VOUT2, 0x00u /* 400 mV */);
	zassert_equal(tps628640_software_enable(&b4f, true), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(fake_tps628640_write_count(0x4Fu, TPS628640_REG_CONTROL), 0u);

	tps628640_deinit(&b44);
	tps628640_deinit(&b48);
	tps628640_deinit(&b4f);
	alp_i2c_close(bus);
}

/* #1165: init() a rail found DISABLED (CONTROL reads back non-zero
 * with SOFTWARE_ENABLE clear) must shadow that state, not silently seed
 * CTRL_DEFAULT (SOFTWARE_ENABLE=1) -- otherwise the next set_fpwm_mode()
 * (a call that has nothing to do with enable) writes the shadow back and
 * re-enables a rail init found off. */
ZTEST(alp_chips, test_tps628640_init_shadows_disabled_control)
{
	tps628640_t b44, b48, b4f;
	fake_tps628640_reset(0x44u);
	fake_tps628640_reset(0x48u);
	fake_tps628640_reset(0x4Fu);
	/* A disabled rail: SOFTWARE_ENABLE (bit5) clear, everything else at
	 * its datasheet default -- readable and non-zero. */
	fake_tps628640_set_reg(0x4Fu, TPS628640_REG_CONTROL, 0x4Fu);

	alp_i2c_t *bus = pmic_bus_open();
	zassert_not_null(bus);
	zassert_ok(tps628640_init(&b44, bus, 0x44u, 1050u));
	zassert_ok(tps628640_init(&b48, bus, 0x48u, 850u));
	zassert_ok(tps628640_init(&b4f, bus, 0x4Fu, 500u));

	zassert_ok(tps628640_set_limits(&b4f, &tps_l4f));
	zassert_ok(tps628640_set_fpwm_mode(&b4f, true));
	zassert_equal(fake_tps628640_get_reg(0x4Fu, TPS628640_REG_CONTROL),
	              0x5Fu,
	              "FPWM bit (0x10) added to the disabled shadow (0x4F); "
	              "SOFTWARE_ENABLE (0x20) must stay clear");

	tps628640_deinit(&b44);
	tps628640_deinit(&b48);
	tps628640_deinit(&b4f);
	alp_i2c_close(bus);
}

/* #7 (verify-review follow-up): a genuine CONTROL == 0x00 read is a real
 * chip state (every bit clear -- SOFTWARE_ENABLE included), not evidence
 * the read secretly failed; init() must shadow it verbatim rather than
 * falling back to CTRL_DEFAULT (SOFTWARE_ENABLE=1). */
ZTEST(alp_chips, test_tps628640_init_shadows_genuine_zero_control)
{
	tps628640_t b4f;
	fake_tps628640_reset(0x4Fu);
	fake_tps628640_set_reg(0x4Fu, TPS628640_REG_CONTROL, 0x00u);

	alp_i2c_t *bus = pmic_bus_open();
	zassert_not_null(bus);
	zassert_ok(tps628640_init(&b4f, bus, 0x4Fu, 500u));

	zassert_ok(tps628640_set_limits(&b4f, &tps_l4f));
	zassert_ok(tps628640_set_fpwm_mode(&b4f, true));
	zassert_equal(fake_tps628640_get_reg(0x4Fu, TPS628640_REG_CONTROL),
	              TPS628640_CTRL_FPWM_MODE,
	              "only the FPWM bit added to a genuinely-zero shadow; "
	              "SOFTWARE_ENABLE must stay clear, not seed CTRL_DEFAULT");

	tps628640_deinit(&b4f);
	alp_i2c_close(bus);
}

/* #7: a FAILED CONTROL read must fail init() outright -- with no live
 * CONTROL state to shadow, guessing CTRL_DEFAULT risks the same
 * silent-re-enable class of bug the disabled-shadow fix above closes. */
ZTEST(alp_chips, test_tps628640_init_fails_when_control_read_fails)
{
	tps628640_t b4f;
	fake_tps628640_reset(0x4Fu);
	fake_tps628640_fail_next_read(0x4Fu, TPS628640_REG_CONTROL);

	alp_i2c_t *bus = pmic_bus_open();
	zassert_not_null(bus);
	/* The VOUT1 ACK-probe still succeeds; only the CONTROL read fails. */
	zassert_equal(tps628640_init(&b4f, bus, 0x4Fu, 500u), ALP_ERR_NOT_READY);

	alp_i2c_close(bus);
}

/* A reset must not spring a previously-disabled rail back to life: the
 * chip's own reset always re-enables the converter (CTRL_DEFAULT has
 * SOFTWARE_ENABLE=1), so the driver must notice the rail was off going in
 * and switch it back off once the reset lands. */
ZTEST(alp_chips, test_tps628640_reset_keeps_a_disabled_rail_disabled)
{
	tps628640_t b44, b48, b4f;
	alp_i2c_t  *bus = tps_setup(&b44, &b48, &b4f);

	zassert_ok(tps628640_set_limits(&b4f, &tps_l4f));
	zassert_ok(tps628640_software_enable(&b4f, false));
	zassert_equal(fake_tps628640_get_reg(0x4Fu, TPS628640_REG_CONTROL), 0x4Fu);

	zassert_ok(tps628640_reset_to_defaults(&b4f));
	zassert_equal(
	    (fake_tps628640_get_reg(0x4Fu, TPS628640_REG_CONTROL) & TPS628640_CTRL_SOFTWARE_ENABLE),
	    0u,
	    "reset must not leave a previously-off rail energized");

	tps628640_deinit(&b44);
	tps628640_deinit(&b48);
	tps628640_deinit(&b4f);
	alp_i2c_close(bus);
}

/* A reset must also refuse to leave the rail energized at an out-of-window
 * VOUT1 -- the chip's reset can revert VOUT1 to its POR default, which may
 * no longer be inside a window installed after that POR value was set.
 * Non-critical b48 (POR VOUT1 0x5A = 850 mV) with 0x44's foreign
 * [1000, 1100] window installed on it exercises that mismatch without
 * hitting the critical-rail NOSUPPORT check_enable() already refuses on. */
ZTEST(alp_chips, test_tps628640_reset_disables_out_of_window_vout)
{
	tps628640_t b44, b48, b4f;
	alp_i2c_t  *bus = tps_setup(&b44, &b48, &b4f);

	zassert_ok(tps628640_set_limits(&b48, &tps_l44));
	zassert_equal(tps628640_reset_to_defaults(&b48), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(
	    (fake_tps628640_get_reg(0x48u, TPS628640_REG_CONTROL) & TPS628640_CTRL_SOFTWARE_ENABLE),
	    0u,
	    "reset must disable a rail it cannot confirm is in-window");

	tps628640_deinit(&b44);
	tps628640_deinit(&b48);
	tps628640_deinit(&b4f);
	alp_i2c_close(bus);
}

/* Same as above, but VOUT1 alone reverts in-window and ONLY VOUT2
 * disagrees -- the VID strap picks which one is live and this driver
 * can't read it, so reset_to_defaults() must check both, not just VOUT1.
 * The fake normally mirrors VOUT2 to VOUT1's POR byte on every reset
 * (documented TBD-verify -- no bench reading of VOUT2 exists), which
 * makes the two registers byte-identical and so never independently
 * out-of-window after a real reset; fake_tps628640_set_vout2_por_independent()
 * is a test-only hook that turns that mirroring off for one instance, so
 * a pre-armed VOUT2 value survives the reset unmirrored and this test can
 * prove the VOUT2 check is real, not just VOUT1 read twice. */
ZTEST(alp_chips, test_tps628640_reset_disables_out_of_window_vout2)
{
	tps628640_t b44, b48, b4f;
	alp_i2c_t  *bus = tps_setup(&b44, &b48, &b4f);

	zassert_ok(tps628640_set_limits(&b4f, &tps_l4f));
	fake_tps628640_set_vout2_por_independent(0x4Fu, true);
	fake_tps628640_set_reg(0x4Fu, TPS628640_REG_VOUT2, 0x00u /* 400 mV, outside [475, 525] */);

	/* VOUT1 still reverts to its in-window POR (0x14 = 500 mV); only the
	 * pre-armed VOUT2 (400 mV) is out of range. */
	zassert_equal(tps628640_reset_to_defaults(&b4f), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(
	    (fake_tps628640_get_reg(0x4Fu, TPS628640_REG_CONTROL) & TPS628640_CTRL_SOFTWARE_ENABLE),
	    0u,
	    "reset must disable when VOUT2 alone is out of window");

	tps628640_deinit(&b44);
	tps628640_deinit(&b48);
	tps628640_deinit(&b4f);
	alp_i2c_close(bus);
}

/* #2 (verify-review follow-up): a later error after the reset write has
 * already landed (a failed post-reset VOUT1 read here) must not leave the
 * rail fail-open -- the chip already re-enabled it at CTRL_DEFAULT before
 * this point.  reset_to_defaults() must attempt the SOFTWARE_ENABLE-clear
 * write best-effort and return the ORIGINAL error, not silently propagate
 * it with the rail still energized and unconfirmed. */
ZTEST(alp_chips, test_tps628640_reset_disables_on_post_reset_read_failure)
{
	tps628640_t b44, b48, b4f;
	alp_i2c_t  *bus = tps_setup(&b44, &b48, &b4f);

	zassert_ok(tps628640_set_limits(&b4f, &tps_l4f));
	fake_tps628640_fail_next_read(0x4Fu, TPS628640_REG_VOUT1);
	zassert_not_equal(tps628640_reset_to_defaults(&b4f), ALP_OK);
	zassert_equal(
	    (fake_tps628640_get_reg(0x4Fu, TPS628640_REG_CONTROL) & TPS628640_CTRL_SOFTWARE_ENABLE),
	    0u,
	    "a failed post-reset VOUT1 read must not leave the rail fail-open");

	tps628640_deinit(&b44);
	tps628640_deinit(&b48);
	tps628640_deinit(&b4f);
	alp_i2c_close(bus);
}

/* A reset re-enables the converter at the datasheet-default CONTROL byte
 * (FPWM off, slowest ramp) -- confirm the driver restores a previously
 * configured FPWM/ramp instead of silently leaving the rail in PFM mode
 * at the slow ramp once it comes back online. */
ZTEST(alp_chips, test_tps628640_reset_preserves_fpwm_and_ramp)
{
	tps628640_t b4f;
	tps628640_t unused_a, unused_b;
	alp_i2c_t  *bus = tps_setup(&unused_a, &unused_b, &b4f);

	zassert_ok(tps628640_set_limits(&b4f, &tps_l4f));
	zassert_ok(tps628640_set_fpwm_mode(&b4f, true));
	zassert_ok(tps628640_set_ramp_speed(&b4f, TPS628640_RAMP_20_MV_PER_US));
	zassert_equal(fake_tps628640_get_reg(0x4Fu, TPS628640_REG_CONTROL),
	              0x7Cu,
	              "FPWM set (bit4), ramp 00 (bits1:0), rest at CTRL_DEFAULT");

	zassert_ok(tps628640_reset_to_defaults(&b4f));
	zassert_equal(fake_tps628640_get_reg(0x4Fu, TPS628640_REG_CONTROL),
	              0x7Cu,
	              "FPWM + ramp restored after the reset, not left at chip default 0x6F");

	tps628640_deinit(&unused_a);
	tps628640_deinit(&unused_b);
	tps628640_deinit(&b4f);
	alp_i2c_close(bus);
}

/* ---- DA9292 CH2 sequence ------------------------------------------- */

/* alp_gpio_open() ids from the overlay's alp,pin-array. */
#define DX_PIN_REQ      4u /* DEEPX_PWR_EN_REQ (P65), input */
#define DX_PIN_CORE_EN  5u /* DEEPX_CORE_0P75_EN (P64), output */
#define DX_PIN_M1_RESET 6u /* M1_RESET (PA6), output, low = asserted */

#define DX_REG_CTRL_01 0x07u
#define DX_CH1_EN      0x01u
#define DX_CH2_EN      0x02u

static const pmic_rail_limit_t dx_m1_limits[DA9292_CH_COUNT]  = V2N_M1_POWER_DA9292_CH_LIMITS_INIT;
static const pmic_rail_limit_t dx_v2n_limits[DA9292_CH_COUNT] = V2N_POWER_DA9292_CH_LIMITS_INIT;

struct dx_rig {
	alp_i2c_t  *bus;
	alp_gpio_t *req, *en, *rst;
	da9292_t    ctx;
};

struct dx_delay {
	uint32_t calls;
	bool     vstep_glitch; /* first delay: CH2 flips to VSTEP=1 + EN=1, request rises */
};

static const struct device *dx_gpio(void)
{
	return DEVICE_DT_GET(DT_NODELABEL(gpio_emul0));
}

static void dx_delay_fn(void *user, uint32_t us)
{
	(void)us;
	struct dx_delay *d = user;
	d->calls++;
	if (d->vstep_glitch && d->calls == 1u) {
		fake_da9292_force_reg(DX_REG_CTRL_01,
		                      (uint8_t)(fake_da9292_get_reg(DX_REG_CTRL_01) | 0x80u | DX_CH2_EN));
		(void)gpio_emul_input_set(dx_gpio(), DX_PIN_REQ, 1);
	}
}

/* Caller resets / seeds the fake first; init only reads. */
static void dx_open(struct dx_rig *r, bool req_level)
{
	r->bus = pmic_bus_open();
	r->req = alp_gpio_open(DX_PIN_REQ);
	r->en  = alp_gpio_open(DX_PIN_CORE_EN);
	r->rst = alp_gpio_open(DX_PIN_M1_RESET);
	zassert_not_null(r->bus);
	zassert_not_null(r->req);
	zassert_not_null(r->en);
	zassert_not_null(r->rst);
	zassert_ok(alp_gpio_configure(r->req, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE));
	zassert_ok(alp_gpio_configure(r->en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE));
	zassert_ok(alp_gpio_configure(r->rst, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE));
	zassert_ok(alp_gpio_write(r->en, false));
	zassert_ok(alp_gpio_write(r->rst, false));
	zassert_ok(gpio_emul_input_set(dx_gpio(), DX_PIN_REQ, req_level ? 1 : 0));
	zassert_ok(da9292_init(&r->ctx, r->bus, DA9292_I2C_ADDR_V2N));
	zassert_ok(da9292_set_limits(&r->ctx, dx_m1_limits));
	fake_da9292_log_reset();
}

static void dx_close(struct dx_rig *r)
{
	da9292_deinit(&r->ctx);
	alp_gpio_close(r->req);
	alp_gpio_close(r->en);
	alp_gpio_close(r->rst);
	alp_i2c_close(r->bus);
}

static struct da9292_ch2_seq_cfg dx_cfg(const struct dx_rig *r, struct dx_delay *d)
{
	return (struct da9292_ch2_seq_cfg){
		.target_mv       = 750u,
		.expected_dev_id = V2N_POWER_DA9292_DEV_ID,
		.pwr_en_req      = r->req,
		.core_en         = r->en,
		.m1_reset        = r->rst,
		.req_timeout_ms  = 500u,
		.pg_timeout_ms   = 20u,
		.pg_settle_ms    = 5u,
		.delay           = dx_delay_fn,
		.delay_user      = d,
	};
}

/* Number of logged CTRL_01 writes whose (val & mask) == want. */
static size_t dx_ctrl_writes(uint8_t mask, uint8_t want)
{
	size_t n = 0;
	for (size_t i = 0; i < fake_da9292_log_len(); i++) {
		const struct fake_da9292_write *w = fake_da9292_log(i);
		if (w->reg == DX_REG_CTRL_01 && (w->val & mask) == want) n++;
	}
	return n;
}

static int dx_pin(uint32_t pin)
{
	return gpio_emul_output_get(dx_gpio(), pin);
}

ZTEST(alp_chips, test_da9292_ch2_sequence_cold_start)
{
	struct dx_rig                r;
	struct dx_delay              d = { 0 };
	struct da9292_ch2_seq_result res;
	fake_da9292_reset(); /* OTP image: CTRL_01 0x81, CH2 0xB4/0x9A at VSTEP=1 */
	dx_open(&r, true);
	const struct da9292_ch2_seq_cfg cfg = dx_cfg(&r, &d);

	zassert_ok(da9292_ch2_sequence(&r.ctx, &cfg, &res));
	zassert_equal(res.step, DA9292_SEQ_OK);
	zassert_false(res.already_programmed);
	zassert_true(res.rail_up);

	zassert_equal(fake_da9292_get_reg(0x0Cu), 0x96u);
	zassert_equal(fake_da9292_get_reg(0x0Du), 0x96u);
	zassert_equal(fake_da9292_write_count(0x0Au), 0u, "CH1 setpoint untouched");
	zassert_equal(fake_da9292_get_reg(DX_REG_CTRL_01), 0x03u);

	/* RMW: clear VSTEP+EN keeping CH1_EN (0x01), then enable (0x03). */
	zassert_equal(dx_ctrl_writes(0x00u, 0x00u), 2u);
	zassert_equal(dx_ctrl_writes(DX_CH1_EN, 0x00u), 0u, "every CTRL_01 write keeps CH1_EN");
	zassert_equal(dx_ctrl_writes(0xFFu, 0x01u), 1u);
	zassert_equal(dx_ctrl_writes(0xFFu, 0x03u), 1u);

	zassert_equal(dx_pin(DX_PIN_CORE_EN), 1, "DEEPX_CORE_0P75_EN driven high on success");
	zassert_equal(dx_pin(DX_PIN_M1_RESET), 0, "the sequence never releases M1_RESET");
	dx_close(&r);
}

/* Warm reboot: CH2 already 0x96/0x96 at VSTEP=0 and LIVE.  The sequence
 * must not reprogram it and must never clear CH2_EN -- the DEEPX may be
 * drawing off it. */
ZTEST(alp_chips, test_da9292_ch2_sequence_warm_keeps_rail_live)
{
	struct dx_rig                r;
	struct dx_delay              d = { 0 };
	struct da9292_ch2_seq_result res;
	fake_da9292_reset();
	fake_da9292_force_reg(DX_REG_CTRL_01, DX_CH1_EN | DX_CH2_EN);
	fake_da9292_force_reg(0x0Cu, 0x96u);
	fake_da9292_force_reg(0x0Du, 0x96u);
	fake_da9292_force_reg(0x00u, 0x03u); /* CH1_PG | CH2_PG */
	dx_open(&r, true);
	const struct da9292_ch2_seq_cfg cfg = dx_cfg(&r, &d);

	zassert_ok(da9292_ch2_sequence(&r.ctx, &cfg, &res));
	zassert_true(res.already_programmed);
	zassert_true(res.rail_up);
	zassert_equal(fake_da9292_write_count(0x0Cu), 0u);
	zassert_equal(fake_da9292_write_count(0x0Du), 0u);
	zassert_equal(dx_ctrl_writes(DX_CH2_EN, 0x00u), 0u, "no CTRL_01 write may clear CH2_EN");
	zassert_equal(dx_ctrl_writes(0x00u, 0x00u), 0u, "CH2_EN already set: zero CTRL_01 writes");
	zassert_equal(fake_da9292_get_reg(DX_REG_CTRL_01), 0x03u);
	dx_close(&r);
}

/* Warm reboot, CH2 live, DEEPX_PWR_EN_REQ never rises: the rail and its
 * enable pin must be left exactly as found (no CTRL_01 write, P64 held). */
ZTEST(alp_chips, test_da9292_ch2_sequence_warm_no_request_leaves_rail)
{
	struct dx_rig                r;
	struct dx_delay              d = { 0 };
	struct da9292_ch2_seq_result res;
	fake_da9292_reset();
	fake_da9292_force_reg(DX_REG_CTRL_01, DX_CH1_EN | DX_CH2_EN);
	fake_da9292_force_reg(0x0Cu, 0x96u);
	fake_da9292_force_reg(0x0Du, 0x96u);
	fake_da9292_force_reg(0x00u, 0x03u);
	dx_open(&r, false);
	zassert_ok(alp_gpio_write(r.en, true)); /* live P64 from the previous boot */
	const struct da9292_ch2_seq_cfg cfg = dx_cfg(&r, &d);

	zassert_equal(da9292_ch2_sequence(&r.ctx, &cfg, &res), ALP_ERR_NOT_READY);
	zassert_equal(res.step, DA9292_SEQ_ERR_NO_REQUEST);
	zassert_equal(dx_ctrl_writes(0x00u, 0x00u), 0u);
	zassert_equal(fake_da9292_get_reg(DX_REG_CTRL_01), 0x03u);
	zassert_equal(dx_pin(DX_PIN_CORE_EN), 1, "P64 not dropped under a live rail");
	dx_close(&r);
}

ZTEST(alp_chips, test_da9292_ch2_sequence_pg_fail_clears_en)
{
	struct dx_rig                r;
	struct dx_delay              d = { 0 };
	struct da9292_ch2_seq_result res;
	fake_da9292_reset();
	fake_da9292_set_pg_delay(UINT32_MAX);
	dx_open(&r, true);
	const struct da9292_ch2_seq_cfg cfg = dx_cfg(&r, &d);

	zassert_equal(da9292_ch2_sequence(&r.ctx, &cfg, &res), ALP_ERR_TIMEOUT);
	zassert_equal(res.step, DA9292_SEQ_ERR_PG_TIMEOUT);
	zassert_false(res.rail_up);
	zassert_equal(fake_da9292_get_reg(DX_REG_CTRL_01), 0x01u, "CH2_EN cleared, CH1_EN kept");
	const struct fake_da9292_write *last = fake_da9292_log(fake_da9292_log_len() - 1u);
	zassert_equal(last->reg, DX_REG_CTRL_01);
	zassert_equal(last->val & DX_CH2_EN, 0u, "last word on the bus disables CH2");
	zassert_equal(dx_pin(DX_PIN_CORE_EN), 0);
	zassert_equal(dx_pin(DX_PIN_M1_RESET), 0);
	dx_close(&r);
}

ZTEST(alp_chips, test_da9292_ch2_sequence_vstep_at_enable_aborts)
{
	struct dx_rig                r;
	struct dx_delay              d = { .vstep_glitch = true };
	struct da9292_ch2_seq_result res;
	fake_da9292_reset();
	dx_open(&r, false); /* request rises inside the first delay, with the glitch */
	const struct da9292_ch2_seq_cfg cfg = dx_cfg(&r, &d);

	zassert_equal(da9292_ch2_sequence(&r.ctx, &cfg, &res), ALP_ERR_IO);
	zassert_equal(res.step, DA9292_SEQ_ERR_VSTEP_AT_ENABLE);
	zassert_equal(dx_ctrl_writes(DX_CH2_EN, DX_CH2_EN), 0u, "never enables into VSTEP=1");
	zassert_equal(fake_da9292_get_reg(DX_REG_CTRL_01) & DX_CH2_EN, 0u, "glitched CH2_EN cleared");
	zassert_equal(dx_pin(DX_PIN_CORE_EN), 0);
	zassert_equal(dx_pin(DX_PIN_M1_RESET), 0);
	dx_close(&r);
}

ZTEST(alp_chips, test_da9292_ch2_sequence_no_request_never_enables)
{
	struct dx_rig                r;
	struct dx_delay              d = { 0 };
	struct da9292_ch2_seq_result res;
	fake_da9292_reset();
	dx_open(&r, false);
	struct da9292_ch2_seq_cfg cfg = dx_cfg(&r, &d);
	cfg.req_timeout_ms            = 3u;

	zassert_equal(da9292_ch2_sequence(&r.ctx, &cfg, &res), ALP_ERR_NOT_READY);
	zassert_equal(res.step, DA9292_SEQ_ERR_NO_REQUEST);
	zassert_equal(dx_ctrl_writes(DX_CH2_EN, DX_CH2_EN), 0u);
	zassert_equal(dx_pin(DX_PIN_CORE_EN), 0);
	dx_close(&r);
}

ZTEST(alp_chips, test_da9292_ch2_sequence_refuses_without_grant)
{
	struct dx_rig                r;
	struct dx_delay              d = { 0 };
	struct da9292_ch2_seq_result res;
	fake_da9292_reset();
	dx_open(&r, true);
	struct da9292_ch2_seq_cfg cfg = dx_cfg(&r, &d);

	zassert_ok(da9292_set_limits(&r.ctx, NULL));
	zassert_equal(da9292_ch2_sequence(&r.ctx, &cfg, &res), ALP_ERR_NOSUPPORT);
	/* v2n base: CH2 grants nothing (no DEEPX populated). */
	zassert_ok(da9292_set_limits(&r.ctx, dx_v2n_limits));
	zassert_equal(da9292_ch2_sequence(&r.ctx, &cfg, &res), ALP_ERR_NOSUPPORT);
	zassert_equal(fake_da9292_log_len(), 0u);

	/* Wrong OTP variant: abort at identity, nothing written. */
	zassert_ok(da9292_set_limits(&r.ctx, dx_m1_limits));
	cfg.expected_dev_id = 0xEBu;
	zassert_equal(da9292_ch2_sequence(&r.ctx, &cfg, &res), ALP_ERR_NOSUPPORT);
	zassert_equal(res.step, DA9292_SEQ_ERR_IDENTITY);
	zassert_equal(fake_da9292_log_len(), 0u);
	zassert_equal(dx_pin(DX_PIN_CORE_EN), 0);
	dx_close(&r);
}

ZTEST(alp_chips, test_da9292_control_writes_are_limit_gated)
{
	struct dx_rig   r;
	da9292_events_t ev;
	fake_da9292_reset();
	fake_da9292_force_reg(DX_REG_CTRL_01, DX_CH1_EN); /* CH2 off, VSTEP=0 */
	fake_da9292_force_reg(0x02u, 0x02u);              /* latched CH2_PG event */
	dx_open(&r, false);

	zassert_ok(da9292_set_limits(&r.ctx, NULL));
	zassert_equal(da9292_set_voltage_mv(&r.ctx, DA9292_CH2, 750u), ALP_ERR_NOSUPPORT);
	zassert_equal(da9292_set_enable(&r.ctx, DA9292_CH2, true), ALP_ERR_NOSUPPORT);
	zassert_equal(da9292_read_and_clear_events(&r.ctx, &ev), ALP_ERR_NOSUPPORT);
	zassert_equal(ev.raw_00, 0x02u, "events reported even when not cleared");
	zassert_equal(fake_da9292_get_reg(0x02u), 0x02u);
	zassert_equal(fake_da9292_log_len(), 0u);

	zassert_ok(da9292_set_limits(&r.ctx, dx_m1_limits));
	/* CH2 window [715, 785]. */
	zassert_equal(da9292_set_voltage_mv(&r.ctx, DA9292_CH2, 800u), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(da9292_set_voltage_mv(&r.ctx, DA9292_CH2, 700u), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(fake_da9292_log_len(), 0u);
	zassert_ok(da9292_set_voltage_mv(&r.ctx, DA9292_CH2, 780u));
	zassert_equal(fake_da9292_get_reg(0x0Cu), 0x9Cu);
	zassert_equal(fake_da9292_get_reg(0x0Du), 0x9Cu);

	/* CH1 (RZ/V2N 0.8 V, critical) grants nothing. */
	zassert_equal(da9292_set_voltage_mv(&r.ctx, DA9292_CH1, 800u), ALP_ERR_NOSUPPORT);
	zassert_equal(da9292_set_enable(&r.ctx, DA9292_CH1, false), ALP_ERR_NOSUPPORT);
	zassert_equal(fake_da9292_get_reg(DX_REG_CTRL_01) & DX_CH1_EN, DX_CH1_EN);

	/* CH2 (non-critical DEEPX core) switches both ways. */
	zassert_ok(da9292_set_enable(&r.ctx, DA9292_CH2, true));
	zassert_equal(fake_da9292_get_reg(DX_REG_CTRL_01), 0x03u);
	zassert_ok(da9292_set_enable(&r.ctx, DA9292_CH2, false));
	zassert_equal(fake_da9292_get_reg(DX_REG_CTRL_01), 0x01u);

	zassert_ok(da9292_read_and_clear_events(&r.ctx, &ev));
	zassert_equal(fake_da9292_get_reg(0x02u), 0x00u, "W1C cleared the event");
	dx_close(&r);
}
