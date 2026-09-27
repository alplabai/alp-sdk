/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small board-support IC smokes: rv3028c7 (RTC), tmp112 (temperature
 * sensor), ina236 (current/voltage/power monitor), eeprom_24c128
 * (24Cxx EEPROM), tcal9538 (8-channel I/O expander).
 */

#include <math.h>

#include <zephyr/ztest.h>

#include "alp/chips/eeprom_24c128.h"
#include "alp/chips/ina236.h"
#include "alp/chips/rv3028c7.h"
#include "alp/chips/tcal9538.h"
#include "alp/chips/tmp112.h"
#include "alp/e1m_pinout.h"
#include "alp/peripheral.h"

#include "fakes.h"

/* ------------------------------------------------------------------ */
/* rv3028c7 -- Micro Crystal RV-3028-C7 RTC                           */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_rv3028c7_init_null_args)
{
	rv3028c7_t ctx;
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	zassert_equal(rv3028c7_init(NULL, bus), ALP_ERR_INVAL);
	zassert_equal(rv3028c7_init(&ctx, NULL), ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_rv3028c7_calls_reject_uninitialised)
{
	rv3028c7_t             ctx   = { 0 };
	rv3028c7_time_t        when  = { .year = 2026, .month = 5, .day = 13 };
	rv3028c7_alarm_match_t match = { .match_minute = true };
	bool                   fired;
	uint8_t                status_seen;

	zassert_equal(rv3028c7_get_time(&ctx, &when), ALP_ERR_NOT_READY);
	zassert_equal(rv3028c7_set_time(&ctx, &when), ALP_ERR_NOT_READY);
	zassert_equal(rv3028c7_set_alarm(&ctx, &when, &match), ALP_ERR_NOT_READY);
	zassert_equal(rv3028c7_alarm_int_enable(&ctx, true), ALP_ERR_NOT_READY);
	zassert_equal(rv3028c7_alarm_check_and_clear(&ctx, &fired), ALP_ERR_NOT_READY);
	zassert_equal(rv3028c7_dispatch_irq(&ctx, &status_seen), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_rv3028c7_register_handler_validates_src)
{
	/* Force .initialised so the function reaches the src-range
     * check before any I2C work. */
	rv3028c7_t ctx = { .initialised = true };

	/* Source value beyond the documented enum (RV3028C7_SRC_COUNT = 7)
     * must be rejected. */
	zassert_equal(rv3028c7_register_handler(&ctx, (rv3028c7_src_t)RV3028C7_SRC_COUNT, NULL, NULL),
	              ALP_ERR_INVAL);

	/* NULL handler is documented as "unregister" -- must NOT be an
     * INVAL.  A valid source + NULL handler should succeed. */
	zassert_equal(rv3028c7_register_handler(&ctx, RV3028C7_SRC_ALARM, NULL, NULL), ALP_OK);
}

/* ------------------------------------------------------------------ */
/* rv3028c7 -- fake-backed register-protocol tests                    */
/* ------------------------------------------------------------------ */
/* fake_rv3028c7.c models the EEADDR(0x25)/EEDATA(0x26)/EECMD(0x27)
 * EEPROM-commit protocol plus an ordered write log -- see that file's
 * header comment.  These exercise rv3028c7_route_clkout()'s endurance
 * guard (rv3028c7.c) end-to-end, which a last-value-only register
 * echo cannot: the guard's whole point is to compare the EEPROM
 * readback against the RAM mirror, not the mirror against itself. */

static alp_i2c_t *open_rv3028c7_bus(void)
{
	return alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
}

ZTEST(alp_chips, test_fake_rv3028c7_route_clkout_skips_commit_when_ee_already_matches)
{
	fake_rv3028c7_reset();
	alp_i2c_t *bus = open_rv3028c7_bus();
	zassert_not_null(bus);

	rv3028c7_t ctx;
	zassert_equal(rv3028c7_init(&ctx, bus), ALP_OK);

	/* EEPROM_CLKOUT (0x35) RAM mirror starts at 0 -> masked new value
	 * for RV3028C7_CLKOUT_8192_HZ (1) is 1.  Pre-seed the EEPROM
	 * backing store (NOT the RAM mirror) to already hold that byte, so
	 * the endurance guard's readback compare finds no difference. */
	fake_rv3028c7_set_eeprom(0x35u, 0x01u);
	fake_rv3028c7_wlog_reset();

	zassert_equal(rv3028c7_route_clkout(&ctx, RV3028C7_CLKOUT_8192_HZ), ALP_OK);

	/* No EECMD=WRITE (0x21) anywhere in the log -- the commit that
	 * would burn an EEPROM write cycle was skipped.  (The readback
	 * protocol itself still touches 0x27 with 0x00/0x22 -- that part
	 * costs no write-cycle endurance per the driver's own comment --
	 * so this checks specifically for the commit opcode, not "0x27
	 * untouched".) */
	for (size_t i = 0; i < fake_rv3028c7_wlog_len(); i++) {
		zassert_false(fake_rv3028c7_wlog_reg(i) == 0x27u && fake_rv3028c7_wlog_val(i) == 0x21u,
		              "EECMD=WRITE must not be issued when the EEPROM already matches");
	}

	rv3028c7_deinit(&ctx);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_rv3028c7_route_clkout_commits_differing_mirror_in_order)
{
	fake_rv3028c7_reset();
	alp_i2c_t *bus = open_rv3028c7_bus();
	zassert_not_null(bus);

	rv3028c7_t ctx;
	zassert_equal(rv3028c7_init(&ctx, bus), ALP_OK);

	/* EEPROM backing store left at its reset default (0) -- differs
	 * from the RV3028C7_CLKOUT_8192_HZ (1) target, so the guard must
	 * commit. */
	fake_rv3028c7_wlog_reset();
	zassert_equal(rv3028c7_route_clkout(&ctx, RV3028C7_CLKOUT_8192_HZ), ALP_OK);

	/* Locate the commit opcode (0x27=0x21) and assert the exact
	 * 4-transaction ordered sequence that precedes it:
	 * 0x25=0x35 (EEADDR), 0x26=<value> (EEDATA), 0x27=0x00 (arm),
	 * 0x27=0x21 (commit). */
	size_t commit_idx = SIZE_MAX;
	for (size_t i = 0; i < fake_rv3028c7_wlog_len(); i++) {
		if (fake_rv3028c7_wlog_reg(i) == 0x27u && fake_rv3028c7_wlog_val(i) == 0x21u) {
			commit_idx = i;
			break;
		}
	}
	zassert_true(commit_idx != SIZE_MAX && commit_idx >= 3, "EECMD=WRITE never issued");

	zassert_equal(fake_rv3028c7_wlog_reg(commit_idx - 3), 0x25u);
	zassert_equal(fake_rv3028c7_wlog_val(commit_idx - 3), 0x35u);
	zassert_equal(fake_rv3028c7_wlog_reg(commit_idx - 2), 0x26u);
	zassert_equal(fake_rv3028c7_wlog_val(commit_idx - 2), 0x01u);
	zassert_equal(fake_rv3028c7_wlog_reg(commit_idx - 1), 0x27u);
	zassert_equal(fake_rv3028c7_wlog_val(commit_idx - 1), 0x00u);
	zassert_equal(fake_rv3028c7_wlog_reg(commit_idx), 0x27u);
	zassert_equal(fake_rv3028c7_wlog_val(commit_idx), 0x21u);

	zassert_equal(
	    fake_rv3028c7_get_eeprom(0x35u), 0x01u, "commit must land in the EEPROM backing store");

	rv3028c7_deinit(&ctx);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_rv3028c7_route_clkout_eebusy_held_times_out_never_writes_eecmd)
{
	fake_rv3028c7_reset();
	alp_i2c_t *bus = open_rv3028c7_bus();
	zassert_not_null(bus);

	rv3028c7_t ctx;
	zassert_equal(rv3028c7_init(&ctx, bus), ALP_OK);

	/* STATUS bit 7 (EEbusy) held set for every read -- the FIRST
	 * busy-wait (right after CONTROL_1's EERD is set, before the RAM
	 * mirror is even read) must time out and short-circuit the whole
	 * EEPROM path. */
	fake_rv3028c7_set_reg(0x0Eu, 0x80u);
	fake_rv3028c7_wlog_reset();

	zassert_equal(rv3028c7_route_clkout(&ctx, RV3028C7_CLKOUT_8192_HZ), ALP_ERR_TIMEOUT);

	for (size_t i = 0; i < fake_rv3028c7_wlog_len(); i++) {
		zassert_not_equal(
		    fake_rv3028c7_wlog_reg(i), 0x27u, "EECMD must never be written while EEbusy is held");
	}

	rv3028c7_deinit(&ctx);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_rv3028c7_route_clkout_retries_commit_after_failure)
{
	fake_rv3028c7_reset();
	alp_i2c_t *bus = open_rv3028c7_bus();
	zassert_not_null(bus);

	rv3028c7_t ctx;
	zassert_equal(rv3028c7_init(&ctx, bus), ALP_OK);

	/* Fail exactly the commit write (EECMD=0x21) on the first attempt.
	 * The RAM mirror write (0x35) still succeeds -- if the endurance
	 * guard wrongly compared against the RAM mirror instead of an
	 * EEPROM readback, this failed-then-retried call would silently
	 * skip the commit forever (the mirror already "matches" on the
	 * retry, even though the EEPROM backing store never got the new
	 * byte). */
	fake_rv3028c7_fail_next_write(0x27u, 0x21u);
	fake_rv3028c7_wlog_reset();

	zassert_not_equal(rv3028c7_route_clkout(&ctx, RV3028C7_CLKOUT_8192_HZ), ALP_OK);
	zassert_equal(fake_rv3028c7_get_eeprom(0x35u), 0x00u, "failed commit must not land in EEPROM");

	/* Retry with the same target -- no fault armed this time. */
	fake_rv3028c7_wlog_reset();
	zassert_equal(rv3028c7_route_clkout(&ctx, RV3028C7_CLKOUT_8192_HZ), ALP_OK);

	size_t commit_idx = SIZE_MAX;
	for (size_t i = 0; i < fake_rv3028c7_wlog_len(); i++) {
		if (fake_rv3028c7_wlog_reg(i) == 0x27u && fake_rv3028c7_wlog_val(i) == 0x21u) {
			commit_idx = i;
			break;
		}
	}
	zassert_true(commit_idx != SIZE_MAX, "retry must re-issue the EECMD=WRITE commit");
	zassert_equal(fake_rv3028c7_get_eeprom(0x35u), 0x01u, "retry must land the byte in EEPROM");

	rv3028c7_deinit(&ctx);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_rv3028c7_init_forces_24h_mode)
{
	fake_rv3028c7_reset();
	alp_i2c_t *bus = open_rv3028c7_bus();
	zassert_not_null(bus);

	/* CONTROL_2 bit 1 (12_24) set going in -- init must clear it
	 * (0 = 24h). */
	fake_rv3028c7_set_reg(0x10u, 0x02u);

	rv3028c7_t ctx;
	zassert_equal(rv3028c7_init(&ctx, bus), ALP_OK);
	zassert_equal(fake_rv3028c7_get_reg(0x10u), 0x00u, "12_24 bit must be cleared by init");

	rv3028c7_deinit(&ctx);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_rv3028c7_was_cold_start_reports_latched_porf)
{
	alp_i2c_t *bus = open_rv3028c7_bus();
	zassert_not_null(bus);

	/* Cold-start case: PORF (STATUS bit 0) set going in. */
	fake_rv3028c7_reset();
	fake_rv3028c7_set_reg(0x0Eu, 0x01u);
	rv3028c7_t ctx;
	zassert_equal(rv3028c7_init(&ctx, bus), ALP_OK);
	bool cold = false;
	zassert_equal(rv3028c7_was_cold_start(&ctx, &cold), ALP_OK);
	zassert_true(cold, "PORF was set at init -- must report a cold start");
	zassert_equal(fake_rv3028c7_get_reg(0x0Eu) & 0x01u, 0u, "PORF must be cleared by init");
	rv3028c7_deinit(&ctx);

	/* Warm-start case: PORF clear going in. */
	fake_rv3028c7_reset();
	fake_rv3028c7_set_reg(0x0Eu, 0x00u);
	zassert_equal(rv3028c7_init(&ctx, bus), ALP_OK);
	cold = true;
	zassert_equal(rv3028c7_was_cold_start(&ctx, &cold), ALP_OK);
	zassert_false(cold, "PORF was clear at init -- must not report a cold start");
	rv3028c7_deinit(&ctx);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_rv3028c7_get_time_all_ff_is_bus_timeout)
{
	fake_rv3028c7_reset();
	alp_i2c_t *bus = open_rv3028c7_bus();
	zassert_not_null(bus);

	rv3028c7_t ctx;
	zassert_equal(rv3028c7_init(&ctx, bus), ALP_OK);

	/* p.53: a stalled transaction reports back all-0xFF, which
	 * decodes to a well-formed-looking but bogus time (second=85). */
	for (uint8_t r = 0x00u; r <= 0x06u; r++) {
		fake_rv3028c7_set_reg(r, 0xFFu);
	}

	rv3028c7_time_t out;
	zassert_equal(rv3028c7_get_time(&ctx, &out), ALP_ERR_IO);

	rv3028c7_deinit(&ctx);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_rv3028c7_get_time_accepts_por_default_weekday)
{
	fake_rv3028c7_reset();
	alp_i2c_t *bus = open_rv3028c7_bus();
	zassert_not_null(bus);

	rv3028c7_t ctx;
	zassert_equal(rv3028c7_init(&ctx, bus), ALP_OK);

	/* Application Manual Rev. 1.4 Sec. 3.4 "03h -- Weekday", p.16:
	 * WEEKDAY is a raw 0..6 counter, and 0 is that register's OWN
	 * POR-reset default -- not a fault code.  A board with no
	 * VBACKUP is a cold start on every power cycle (PORF always
	 * latched), so weekday genuinely reads 0 while seconds/minutes/
	 * hours are perfectly valid and advancing.  A prior 1..7 bound
	 * on this check rejected exactly this reading and threw away
	 * the whole 7-byte decode -- on real E1M-AEN803 silicon that
	 * looked like a stopped clock (aen-evk-demo phase_rtc_temp()
	 * reported "t0=00:00:00 t1=00:00:00 advanced=no") while a
	 * direct I2C census on the same board watched the raw seconds
	 * register measurably advance from 0x20 to 0x21.  Assert on the
	 * DECODED FIELDS below, not just the return code: the bug's
	 * signature was a caller struct left untouched while
	 * get_time() still reported success -- a bare ALP_OK check
	 * would pass against that broken driver too. */
	fake_rv3028c7_set_reg(0x00u, 0x12u); /* seconds = 12 (BCD) */
	fake_rv3028c7_set_reg(0x01u, 0x34u); /* minutes = 34 (BCD) */
	fake_rv3028c7_set_reg(0x02u, 0x05u); /* hours   = 5  (BCD) */
	fake_rv3028c7_set_reg(0x03u, 0x00u); /* weekday = 0  (raw, POR default) */
	fake_rv3028c7_set_reg(0x04u, 0x09u); /* day     = 9  (BCD) */
	fake_rv3028c7_set_reg(0x05u, 0x03u); /* month   = 3  (BCD) */
	fake_rv3028c7_set_reg(0x06u, 0x26u); /* year    = 26 (BCD) -> 2026 */

	rv3028c7_time_t out = { 0 };
	zassert_equal(rv3028c7_get_time(&ctx, &out), ALP_OK);
	zassert_equal(out.second, 12u);
	zassert_equal(out.minute, 34u);
	zassert_equal(out.hour, 5u);
	zassert_equal(out.weekday, 0u);
	zassert_equal(out.day, 9u);
	zassert_equal(out.month, 3u);
	zassert_equal(out.year, 2026u);

	rv3028c7_deinit(&ctx);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_rv3028c7_get_time_weekday_bound_is_0_to_6)
{
	fake_rv3028c7_reset();
	alp_i2c_t *bus = open_rv3028c7_bus();
	zassert_not_null(bus);

	rv3028c7_t ctx;
	zassert_equal(rv3028c7_init(&ctx, bus), ALP_OK);

	/* Same Sec. 3.4 p.16 bound, the other end of the range: 6 is
	 * the counter's last legal value and must be accepted; 7 can
	 * never occur on real silicon (the retired 1..7 bound let it
	 * through) and must still be rejected. */
	fake_rv3028c7_set_reg(0x00u, 0x12u);
	fake_rv3028c7_set_reg(0x01u, 0x34u);
	fake_rv3028c7_set_reg(0x02u, 0x05u);
	fake_rv3028c7_set_reg(0x04u, 0x09u);
	fake_rv3028c7_set_reg(0x05u, 0x03u);
	fake_rv3028c7_set_reg(0x06u, 0x26u);

	fake_rv3028c7_set_reg(0x03u, 0x06u);
	rv3028c7_time_t out = { 0 };
	zassert_equal(rv3028c7_get_time(&ctx, &out), ALP_OK);
	zassert_equal(out.weekday, 6u);

	fake_rv3028c7_set_reg(0x03u, 0x07u);
	zassert_equal(rv3028c7_get_time(&ctx, &out), ALP_ERR_IO);

	rv3028c7_deinit(&ctx);
	alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* tmp112 -- TI temperature sensor                                    */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_tmp112_init_null_args)
{
	tmp112_t   ctx;
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	zassert_equal(tmp112_init(NULL, bus, TMP112_I2C_ADDR_GND), ALP_ERR_INVAL);
	zassert_equal(tmp112_init(&ctx, NULL, TMP112_I2C_ADDR_GND), ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_tmp112_calls_reject_uninitialised)
{
	tmp112_t ctx = { 0 };
	int32_t  temp_mc;

	zassert_equal(tmp112_set_rate(&ctx, TMP112_RATE_4_HZ), ALP_ERR_NOT_READY);
	zassert_equal(tmp112_set_extended_mode(&ctx, false), ALP_ERR_NOT_READY);
	zassert_equal(tmp112_read_temp_milli_c(&ctx, &temp_mc), ALP_ERR_NOT_READY);

	tmp112_deinit(&ctx);
	tmp112_deinit(NULL);
}

/* ------------------------------------------------------------------ */
/* ina236 -- TI current / voltage / power monitor                     */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_ina236_init_null_args)
{
	/* The init signature requires several extra calibration
     * parameters; check NULL ctx / NULL bus + invalid shunt
     * resistance are all rejected.  The driver accepts addr_7bit == 0
     * as "fall back to default" so don't test that as INVAL. */
	/* Forward-declare the init signature locally so the test
     * compiles even if the header's extra args shift around;
     * this matches the real signature documented in ina236.h. */
	extern alp_status_t ina236_init(ina236_t * ctx,
	                                alp_i2c_t * bus,
	                                uint8_t           addr_7bit,
	                                float             shunt_ohms,
	                                float             max_current_a,
	                                ina236_adcrange_t adcrange);
	ina236_t            ctx;
	alp_i2c_t          *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	zassert_equal(ina236_init(NULL, bus, 0x40u, 0.010f, 1.0f, INA236_ADCRANGE_81MV), ALP_ERR_INVAL);
	zassert_equal(ina236_init(&ctx, NULL, 0x40u, 0.010f, 1.0f, INA236_ADCRANGE_81MV),
	              ALP_ERR_INVAL);
	/* shunt_ohms <= 0 must be rejected (datasheet's CURRENT_LSB
     * formula divides by it). */
	zassert_equal(ina236_init(&ctx, bus, 0x40u, 0.0f, 1.0f, INA236_ADCRANGE_81MV), ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

/* #739: address must fall inside the INA236A/B strap ranges (0x40..0x43,
 * 0x48..0x4B); 0 is the documented "use default" sentinel and stays
 * legal.  #757: NaN/Inf shunt_ohms / max_current_a must be rejected
 * instead of silently passing the old `<= 0.0f` check (NaN compares
 * false against every relop) and landing in the CALIBRATION register
 * cast. */
ZTEST(alp_chips, test_ina236_init_validates_address_and_numeric_edges)
{
	extern alp_status_t ina236_init(ina236_t * ctx,
	                                alp_i2c_t * bus,
	                                uint8_t           addr_7bit,
	                                float             shunt_ohms,
	                                float             max_current_a,
	                                ina236_adcrange_t adcrange);
	ina236_t            ctx;
	alp_i2c_t          *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	/* addr_7bit == 0 must stay legal (falls back to 0x40). */
	zassert_not_equal(ina236_init(&ctx, bus, 0x00u, 0.010f, 1.0f, INA236_ADCRANGE_81MV),
	                  ALP_ERR_INVAL,
	                  "addr=0 is the documented default sentinel, must not be INVAL");

	/* Boundary probes: 0x7F, 0x80, 0xFF and the gap between the two
	 * strap bands (0x44..0x47) are all out of range. */
	const uint8_t bad_addr[] = { 0x44u, 0x47u, 0x7Fu, 0x80u, 0xFFu };
	for (size_t i = 0; i < ARRAY_SIZE(bad_addr); ++i) {
		zassert_equal(ina236_init(&ctx, bus, bad_addr[i], 0.010f, 1.0f, INA236_ADCRANGE_81MV),
		              ALP_ERR_INVAL,
		              "addr 0x%02x must be rejected",
		              bad_addr[i]);
	}

	/* NaN / Inf numeric edges. */
	const float nan_v = NAN;
	const float inf_v = INFINITY;
	zassert_equal(ina236_init(&ctx, bus, 0x40u, nan_v, 1.0f, INA236_ADCRANGE_81MV),
	              ALP_ERR_INVAL,
	              "NaN shunt_ohms must be rejected");
	zassert_equal(ina236_init(&ctx, bus, 0x40u, inf_v, 1.0f, INA236_ADCRANGE_81MV),
	              ALP_ERR_INVAL,
	              "Inf shunt_ohms must be rejected");
	zassert_equal(ina236_init(&ctx, bus, 0x40u, 0.010f, nan_v, INA236_ADCRANGE_81MV),
	              ALP_ERR_INVAL,
	              "NaN max_current_a must be rejected");
	zassert_equal(ina236_init(&ctx, bus, 0x40u, 0.010f, inf_v, INA236_ADCRANGE_81MV),
	              ALP_ERR_INVAL,
	              "Inf max_current_a must be rejected");

	alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* ina236 -- fake-backed register-protocol test                       */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_fake_ina236_conversion_ready_issues_a_fresh_read_each_call)
{
	fake_ina236_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	ina236_t ctx;
	zassert_equal(ina236_init(&ctx, bus, 0x40u, 0.010f, 1.0f, INA236_ADCRANGE_81MV), ALP_OK);

	/* CVRF (Mask/Enable bit 3) set -- and clears on the READ ITSELF on
	 * real silicon, so this fake models that by having the test flip
	 * it, not the driver. */
	fake_ina236_set_reg(0x06u, 0x0008u);
	bool ready = false;
	zassert_equal(ina236_conversion_ready(&ctx, &ready), ALP_OK);
	zassert_true(ready);

	fake_ina236_set_reg(0x06u, 0x0000u);
	zassert_equal(ina236_conversion_ready(&ctx, &ready), ALP_OK);
	zassert_false(ready, "second call must see the post-clear value, not a cached true");

	/* Two separate reads of 0x06 -- proves no caching papered over the
	 * clear-on-read semantics. */
	zassert_equal(fake_ina236_read_count(0x06u), 2u);

	ina236_deinit(&ctx);
	alp_i2c_close(bus);
}

/* Regression: a NULL ready_out is a caller-error (ALP_ERR_INVAL), distinct
 * from an uninitialised/NULL ctx (ALP_ERR_NOT_READY) -- these two return
 * codes were briefly folded into one ALP_ERR_NOT_READY while merging in the
 * energy-sampler additions; keep them apart. */
ZTEST(alp_chips, test_ina236_conversion_ready_null_args)
{
	ina236_t ctx_uninit = { 0 };
	bool     ready      = false;

	zassert_equal(ina236_conversion_ready(NULL, &ready), ALP_ERR_NOT_READY);
	zassert_equal(ina236_conversion_ready(&ctx_uninit, &ready), ALP_ERR_NOT_READY);

	fake_ina236_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	ina236_t ctx;
	zassert_equal(ina236_init(&ctx, bus, 0x40u, 0.010f, 1.0f, INA236_ADCRANGE_81MV), ALP_OK);

	zassert_equal(ina236_conversion_ready(&ctx, NULL), ALP_ERR_INVAL);

	ina236_deinit(&ctx);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_ina236_read_power_raw_and_uw_match_power_lsb)
{
	fake_ina236_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	/* E1M EVK +5V rail on the FINE range: 20 mOhm, 4.0 A request clamps
	 * to the 1.024 A ceiling, giving the ADC-matched 31.25 uA CURRENT_LSB
	 * and, per eq. 4, exactly 1.0 mW per POWER count (see
	 * test_ina236_calibration_matches_datasheet_and_survives_clamp). */
	ina236_t ctx;
	zassert_equal(ina236_init(&ctx, bus, 0x40u, 0.020f, 4.0f, INA236_ADCRANGE_20MV), ALP_OK);
	zassert_within(ina236_power_lsb_w(&ctx), 0.001f, 1e-9f, "1.0 mW per POWER count");

	fake_ina236_set_reg(0x03u, 1000u);

	uint16_t raw = 0;
	zassert_equal(ina236_read_power_raw(&ctx, &raw), ALP_OK);
	zassert_equal(raw, 1000u);

	/* ina236_read_power_uw() is routed through ina236_power_lsb_w() (no
	 * inline "32.0f * current_lsb_a" restatement) -- this pins that the
	 * two agree: 1000 counts x 1.0 mW/count = 1000 mW = 1,000,000 uW. */
	uint32_t uw = 0;
	zassert_equal(ina236_read_power_uw(&ctx, &uw), ALP_OK);
	zassert_equal(uw, 1000000u, "1000 x 1.0 mW/count = 1,000,000 uW, got %u", uw);

	/* NULL / uninitialised guards on both. */
	ina236_t ctx_uninit = { 0 };
	zassert_equal(ina236_read_power_raw(&ctx_uninit, &raw), ALP_ERR_NOT_READY);
	zassert_equal(ina236_read_power_raw(&ctx, NULL), ALP_ERR_NOT_READY);
	zassert_equal(ina236_read_power_uw(&ctx_uninit, &uw), ALP_ERR_NOT_READY);
	zassert_equal(ina236_read_power_uw(&ctx, NULL), ALP_ERR_NOT_READY);

	ina236_deinit(&ctx);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_ina236_configure_writes_avg_ct_mode_preserving_adcrange)
{
	fake_ina236_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	/* Init on the FINE range so ADCRANGE (CONFIG bit 12) is SET, and prove
	 * ina236_configure() preserves it -- the field it must read-modify-write
	 * around, per its Doxygen. */
	ina236_t ctx;
	zassert_equal(ina236_init(&ctx, bus, 0x40u, 0.010f, 1.0f, INA236_ADCRANGE_20MV), ALP_OK);
	zassert_equal(fake_ina236_get_reg(0x00u) & 0x1000u, 0x1000u, "ADCRANGE bit must be set");

	zassert_equal(
	    ina236_configure(
	        &ctx, INA236_AVG_16, INA236_CT_204US, INA236_CT_332US, INA236_MODE_SHUNT_BUS_CONT),
	    ALP_OK);

	/* AVG=16(code 2)<<9 | VBUSCT=204us(code 1)<<6 | VSHCT=332us(code 2)<<3 |
	 * MODE=shunt+bus-cont(code 7), ADCRANGE (bit 12) preserved from init. */
	uint16_t expect = 0x1000u | (2u << 9) | (1u << 6) | (2u << 3) | 7u;
	zassert_equal(fake_ina236_get_reg(0x00u),
	              expect,
	              "CONFIG = 0x%04x, expected 0x%04x",
	              fake_ina236_get_reg(0x00u),
	              expect);

	/* Out-of-range fields are rejected before any bus write. */
	uint32_t writes_before = fake_ina236_read_count(0x00u); /* unrelated counter, just a marker */
	(void)writes_before;
	zassert_equal(
	    ina236_configure(
	        &ctx, (ina236_avg_t)8, INA236_CT_204US, INA236_CT_332US, INA236_MODE_SHUNT_BUS_CONT),
	    ALP_ERR_INVAL);

	ina236_t ctx_uninit = { 0 };
	zassert_equal(ina236_configure(&ctx_uninit,
	                               INA236_AVG_16,
	                               INA236_CT_204US,
	                               INA236_CT_332US,
	                               INA236_MODE_SHUNT_BUS_CONT),
	              ALP_ERR_NOT_READY);

	ina236_deinit(&ctx);
	alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* ina236 -- energy-sampler helpers (pure functions, no bus traffic)  */
/* ------------------------------------------------------------------ */

/* The POWER scaling had been wrong by a factor of 625 (it applied the
 * bus-voltage LSB a second time on top of the 32 that already carries it),
 * and nothing caught it because every ina236 test above only checks
 * argument rejection.  Both new pure helpers are bus-free, so the scaling
 * and the conversion-timing table CAN be pinned without the (dormant)
 * chips fake layer -- which is exactly what these two tests do. */
ZTEST(alp_chips, test_ina236_power_lsb_is_32x_current_lsb)
{
	/* SBOSA81D eq. 4: Power [W] = 32 x CURRENT_LSB x POWER.  The 32 is
	 * NOT dimensionless -- the internal register math divides by 20000
	 * and 20000 x 1.6 mV (the bus LSB) = 32 V -- so the bus LSB must NOT
	 * be applied again.  Filling current_lsb_a directly keeps this a pure
	 * scaling check with no bus traffic. */
	ina236_t ctx = { 0 };

	/* The +5V EVK rail: 20 mOhm shunt, 4.0 A full scale ->
	 * CURRENT_LSB = 4.0 / 32768 = 122.070312 uA. */
	ctx.current_lsb_a = 4.0f / 32768.0f;
	zassert_within(ina236_power_lsb_w(&ctx),
	               32.0f * (4.0f / 32768.0f),
	               1e-9f,
	               "power LSB must be exactly 32 x CURRENT_LSB (eq. 4)");
	/* Anchor the absolute value too, so a future refactor that keeps the
	 * relationship but rescales CURRENT_LSB still trips: 3.906250 mW. */
	zassert_within(ina236_power_lsb_w(&ctx), 0.00390625f, 1e-9f, "expected 3.90625 mW per count");

	/* A NULL context must yield 0, not read through the pointer. */
	zassert_equal(ina236_power_lsb_w(NULL), 0.0f);
}

/* CURRENT_LSB is a REPORTING scale and must never be derived from a current
 * the selected ADCRANGE cannot measure -- doing so reports every measurable
 * current more coarsely than the ADC resolved it.  The +5V EVK rail is the
 * live case: board data rates it 4.0 A, but on the +/-20.48 mV range a 20 mOhm
 * shunt saturates at 1.024 A. */
ZTEST(alp_chips, test_ina236_full_scale_is_range_over_shunt)
{
	/* SBOSA81D table 7-4 full scales over the shunt: this is the real
	 * measurement ceiling, above which the ADC saturates regardless of the
	 * requested reporting scale. */
	zassert_within(ina236_full_scale_a(0.020f, INA236_ADCRANGE_81MV), 4.096f, 1e-4f);
	zassert_within(ina236_full_scale_a(0.020f, INA236_ADCRANGE_20MV), 1.024f, 1e-4f);
	zassert_within(ina236_full_scale_a(0.050f, INA236_ADCRANGE_20MV), 0.4096f, 1e-5f);
	/* Non-positive / non-finite shunt must not divide by zero. */
	zassert_equal(ina236_full_scale_a(0.0f, INA236_ADCRANGE_81MV), 0.0f);
	zassert_equal(ina236_full_scale_a(NAN, INA236_ADCRANGE_81MV), 0.0f);
}

/* ina236_sample_period_us() (tested below) is derived from these two lookup
 * tables -- pin them directly too, so a table typo shows up at the smallest
 * possible unit rather than only as an already-multiplied period. */
ZTEST(alp_chips, test_ina236_avg_count_and_conversion_time_us_match_datasheet_tables)
{
	/* SBOSA81D table 7-4 AVG field: 1, 4, 16, 64, 128, 256, 512, 1024. */
	zassert_equal(ina236_avg_count(INA236_AVG_1), 1u);
	zassert_equal(ina236_avg_count(INA236_AVG_4), 4u);
	zassert_equal(ina236_avg_count(INA236_AVG_16), 16u);
	zassert_equal(ina236_avg_count(INA236_AVG_64), 64u);
	zassert_equal(ina236_avg_count(INA236_AVG_128), 128u);
	zassert_equal(ina236_avg_count(INA236_AVG_256), 256u);
	zassert_equal(ina236_avg_count(INA236_AVG_512), 512u);
	zassert_equal(ina236_avg_count(INA236_AVG_1024), 1024u);
	zassert_equal(ina236_avg_count((ina236_avg_t)8), 0u, "out-of-range code must report 0");

	/* SBOSA81D table 7-4 VBUSCT/VSHCT field (shared encoding): 140, 204,
	 * 332, 588, 1100, 2116, 4156, 8244 us. */
	zassert_equal(ina236_conversion_time_us(INA236_CT_140US), 140u);
	zassert_equal(ina236_conversion_time_us(INA236_CT_204US), 204u);
	zassert_equal(ina236_conversion_time_us(INA236_CT_332US), 332u);
	zassert_equal(ina236_conversion_time_us(INA236_CT_588US), 588u);
	zassert_equal(ina236_conversion_time_us(INA236_CT_1100US), 1100u);
	zassert_equal(ina236_conversion_time_us(INA236_CT_2116US), 2116u);
	zassert_equal(ina236_conversion_time_us(INA236_CT_4156US), 4156u);
	zassert_equal(ina236_conversion_time_us(INA236_CT_8244US), 8244u);
	zassert_equal(ina236_conversion_time_us((ina236_ct_t)8), 0u, "out-of-range code must report 0");
}

/* The calibration arithmetic is where BOTH shipped scaling bugs lived (power
 * 625x low, SHUNT_CAL not /4 on the fine range) and where a third hid (a
 * saturating clamp that left CURRENT_LSB describing a register value that was
 * never written).  ina236_calibration_for() is the exact function
 * apply_calibration() calls, so these assertions exercise production code
 * rather than restating it -- the earlier version of this test re-derived the
 * expression locally and would still have passed with the clamp reverted. */
ZTEST(alp_chips, test_ina236_calibration_matches_datasheet_and_survives_clamp)
{
	uint16_t cal = 0;
	float    lsb = 0.0f;

	/* E1M EVK +5V rail on the COARSE range: 20 mOhm, rated 4.0 A.  Requested
	 * scale is just under the 4.096 A ceiling, so it is honoured.
	 * SHUNT_CAL = 0.00512 / (CURRENT_LSB x R) rounds 2097.15 -> 2097, and
	 * CURRENT_LSB is then back-derived from 2097. */
	zassert_equal(ina236_calibration_for(0.020f, 4.0f, INA236_ADCRANGE_81MV, &cal, &lsb), ALP_OK);
	zassert_equal(cal, 2097, "eq. 1 rounds to 2097, got %u", cal);
	zassert_within(lsb, 0.00512f / (2097.0f * 0.020f), 1e-9f);

	/* Same rail on the FINE range.  The 4.0 A request exceeds the 1.024 A
	 * the range can measure, so it clamps -- giving the ADC-matched
	 * 31.25 uA and, per eq. 4, exactly 1.0 mW per POWER count.  This is the
	 * configuration the bench measurement ran at. */
	zassert_equal(ina236_calibration_for(0.020f, 4.0f, INA236_ADCRANGE_20MV, &cal, &lsb), ALP_OK);
	zassert_equal(cal, 2048, "0.00512/(31.25uA x 20mOhm)/4 = 2048, got %u", cal);
	zassert_within(lsb, 31.25e-6f, 1e-10f, "ADC-matched CURRENT_LSB");
	zassert_within(32.0f * lsb, 0.001f, 1e-9f, "1.0 mW per POWER count (eq. 4)");

	/* A DELIBERATELY tighter scale than the range is honoured, not widened. */
	zassert_equal(ina236_calibration_for(0.020f, 0.5f, INA236_ADCRANGE_81MV, &cal, &lsb), ALP_OK);
	zassert_true(lsb < 31.25e-6f, "a narrow request must give a finer LSB");

	/* SATURATION: 20 mOhm at 0.2 A wants SHUNT_CAL 41943, above the 15-bit
	 * 0x7FFF ceiling.  The clamp must be reflected in CURRENT_LSB -- keeping
	 * the requested 0.2/32768 = 6.1035 uA would make every current and power
	 * reading 21.9 % low while still returning ALP_OK. */
	zassert_equal(ina236_calibration_for(0.020f, 0.2f, INA236_ADCRANGE_81MV, &cal, &lsb), ALP_OK);
	zassert_equal(cal, 32767, "must saturate at the 15-bit maximum, got %u", cal);
	zassert_within(lsb,
	               0.00512f / (32767.0f * 0.020f),
	               1e-10f,
	               "CURRENT_LSB must follow the CLAMPED SHUNT_CAL");
	zassert_true(lsb > 6.2e-6f, "must NOT keep the unclamped 6.1035 uA");

	/* INVARIANT: SHUNT_CAL always lands in [2048, 32767].  2048 is the
	 * ADC-matched floor -- clamping the scale to the range's full scale FS/R
	 * makes (scale/32768) x R = FS/32768, which is independent of R, so
	 * cal = 0.00512 x 32768 / FS / range_div = 2048 on BOTH ranges.  Any
	 * narrower request only pushes cal up, toward the 15-bit ceiling.  This
	 * is what makes the SHUNT_CAL=0 case ("report zero current and power")
	 * unreachable rather than merely unlikely.  Sweep a wide spread of
	 * shunts and scales, including absurd ones, and prove the bound holds. */
	const float             shunts[] = { 0.001f, 0.020f, 0.050f, 0.500f, 1000.0f };
	const float             scales[] = { 0.001f, 0.2f, 1.0f, 4.0f, 1000.0f };
	const ina236_adcrange_t ranges[] = { INA236_ADCRANGE_81MV, INA236_ADCRANGE_20MV };
	for (size_t r = 0; r < ARRAY_SIZE(shunts); ++r) {
		for (size_t s = 0; s < ARRAY_SIZE(scales); ++s) {
			for (size_t g = 0; g < ARRAY_SIZE(ranges); ++g) {
				zassert_equal(ina236_calibration_for(shunts[r], scales[s], ranges[g], &cal, &lsb),
				              ALP_OK,
				              "R=%f scale=%f range=%d",
				              (double)shunts[r],
				              (double)scales[s],
				              (int)ranges[g]);
				zassert_between_inclusive(cal,
				                          2048,
				                          32767,
				                          "SHUNT_CAL %u out of [2048,32767] for "
				                          "R=%f scale=%f range=%d",
				                          cal,
				                          (double)shunts[r],
				                          (double)scales[s],
				                          (int)ranges[g]);
				zassert_true(lsb > 0.0f);
			}
		}
	}

	/* Argument guards. */
	zassert_equal(ina236_calibration_for(0.020f, 1.0f, INA236_ADCRANGE_81MV, NULL, &lsb),
	              ALP_ERR_INVAL);
	zassert_equal(ina236_calibration_for(0.020f, 1.0f, INA236_ADCRANGE_81MV, &cal, NULL),
	              ALP_ERR_INVAL);
	zassert_equal(ina236_calibration_for(0.0f, 1.0f, INA236_ADCRANGE_81MV, &cal, &lsb),
	              ALP_ERR_INVAL);
	zassert_equal(ina236_calibration_for(0.020f, NAN, INA236_ADCRANGE_81MV, &cal, &lsb),
	              ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_ina236_sample_period_matches_datasheet_tables)
{
	/* SBOSA81D table 7-4 conversion times, section 7.4.4 averaging.  In
	 * continuous shunt+bus the ADC is multiplexed across both, so the two
	 * conversion times ADD; a single-quantity mode counts only its own. */
	zassert_equal(ina236_sample_period_us(
	                  INA236_AVG_1, INA236_CT_140US, INA236_CT_140US, INA236_MODE_SHUNT_BUS_CONT),
	              280u,
	              "1 x (140 + 140) us");
	/* The configuration this repo's energy runner uses: 16 x 280 us. */
	zassert_equal(ina236_sample_period_us(
	                  INA236_AVG_16, INA236_CT_140US, INA236_CT_140US, INA236_MODE_SHUNT_BUS_CONT),
	              4480u,
	              "16 x (140 + 140) us");
	/* Reset defaults: AVG=1, both CT = 1100 us. */
	zassert_equal(ina236_sample_period_us(
	                  INA236_AVG_1, INA236_CT_1100US, INA236_CT_1100US, INA236_MODE_SHUNT_BUS_CONT),
	              2200u);
	/* Shunt-only ignores VBUSCT; bus-only ignores VSHCT. */
	zassert_equal(ina236_sample_period_us(
	                  INA236_AVG_4, INA236_CT_8244US, INA236_CT_204US, INA236_MODE_SHUNT_CONT),
	              816u,
	              "4 x 204 us -- VBUSCT must not count in shunt-only mode");
	zassert_equal(ina236_sample_period_us(
	                  INA236_AVG_4, INA236_CT_204US, INA236_CT_8244US, INA236_MODE_BUS_CONT),
	              816u,
	              "4 x 204 us -- VSHCT must not count in bus-only mode");
	/* BOTH datasheet shutdown encodings (000b and 100b) must report 0 --
	 * no conversions happen, and 0 makes a "divide by the period" caller
	 * fail loudly instead of reporting an impossible sample rate. */
	zassert_equal(ina236_sample_period_us(
	                  INA236_AVG_1, INA236_CT_140US, INA236_CT_140US, INA236_MODE_SHUTDOWN),
	              0u);
	zassert_equal(ina236_sample_period_us(
	                  INA236_AVG_1, INA236_CT_140US, INA236_CT_140US, INA236_MODE_SHUTDOWN_ALT),
	              0u);
	/* Largest legal combination must not overflow: 1024 x (8244 + 8244). */
	zassert_equal(
	    ina236_sample_period_us(
	        INA236_AVG_1024, INA236_CT_8244US, INA236_CT_8244US, INA236_MODE_SHUNT_BUS_CONT),
	    16883712u);
}

/* ------------------------------------------------------------------ */
/* eeprom_24c128 -- generic 24Cxx I2C EEPROM                          */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_eeprom_24c128_init_null_args)
{
	eeprom_24c128_t ctx;
	alp_i2c_t      *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	zassert_equal(eeprom_24c128_init(NULL, bus, EEPROM_24C128_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(eeprom_24c128_init(&ctx, NULL, EEPROM_24C128_I2C_ADDR_LOW), ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_eeprom_24c128_io_rejects_uninitialised)
{
	eeprom_24c128_t ctx         = { 0 };
	uint8_t         scratch[16] = { 0 };

	zassert_equal(eeprom_24c128_read(&ctx, 0u, scratch, sizeof scratch), ALP_ERR_NOT_READY);
	zassert_equal(eeprom_24c128_write(&ctx, 0u, scratch, sizeof scratch), ALP_ERR_NOT_READY);

	eeprom_24c128_deinit(&ctx);
	eeprom_24c128_deinit(NULL);
}

ZTEST(alp_chips, test_eeprom_24c128_io_validates_range)
{
	/* Force .initialised so the function reaches the bounds check. */
	eeprom_24c128_t ctx         = { .initialised = true };
	uint8_t         scratch[16] = { 0 };

	/* Read past the end of the device (16 KB).  Driver reports
     * OUT_OF_RANGE because the offset+len addresses a region the
     * chip doesn't have. */
	zassert_equal(eeprom_24c128_read(&ctx, EEPROM_24C128_BYTES - 8u, scratch, 16u),
	              ALP_ERR_OUT_OF_RANGE);
	/* Write past the end. */
	zassert_equal(eeprom_24c128_write(&ctx, EEPROM_24C128_BYTES - 8u, scratch, 16u),
	              ALP_ERR_OUT_OF_RANGE);
	/* NULL data buffer with non-zero length -> INVAL.  (NULL +
     * zero-length is a documented no-op short-circuit). */
	zassert_equal(eeprom_24c128_read(&ctx, 0u, NULL, 8u), ALP_ERR_INVAL);
	zassert_equal(eeprom_24c128_write(&ctx, 0u, NULL, 8u), ALP_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* tcal9538 -- TI 8-channel I2C I/O expander                          */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_tcal9538_init_null_args)
{
	tcal9538_t ctx;
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	zassert_equal(tcal9538_init(NULL, bus, TCAL9538_I2C_ADDR_BASE), ALP_ERR_INVAL);
	zassert_equal(tcal9538_init(&ctx, NULL, TCAL9538_I2C_ADDR_BASE), ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

/* #739: address must fall inside either the TCA9538/TCAL9538 A1A0
 * strap range (0x70..0x73) or the register-compatible TCA6408A/
 * PCA9538 alt-part's single-A0 strap range (0x20..0x21 -- E1M EVK's
 * TCA6408A alt-population, EVK_I2C_ADDR_TCA6408A_MAIN_NOT_ASSEMBLED on
 * the current EVK revision, alp-sdk#1974/#1980); 0 is the
 * documented "use base" sentinel and stays legal. */
ZTEST(alp_chips, test_tcal9538_init_validates_address_strap_range)
{
	tcal9538_t ctx;
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	zassert_not_equal(tcal9538_init(&ctx, bus, 0x00u),
	                  ALP_ERR_INVAL,
	                  "addr=0 is the documented base-address sentinel, must not be INVAL");

	const uint8_t bad_addr[] = { 0x1Fu, 0x22u, 0x6Fu, 0x74u, 0x7Fu, 0x80u, 0xFFu };
	for (size_t i = 0; i < ARRAY_SIZE(bad_addr); ++i) {
		zassert_equal(tcal9538_init(&ctx, bus, bad_addr[i]),
		              ALP_ERR_INVAL,
		              "addr 0x%02x must be rejected",
		              bad_addr[i]);
	}

	alp_i2c_close(bus);
}

/* Regression for the reviewed fix: a strap-range check that only
 * admits 0x70..0x73 silently breaks every E1M EVK assembled with the
 * TCA6408A alt-population (EVK_I2C_ADDR_TCA6408A_MAIN_NOT_ASSEMBLED
 * = 0x20, bench-confirmed) -- the exact case examples/peripheral-io/i2c-device-hub
 * probes. This bus is backed by native_sim's i2c-emul controller with
 * no fake TCA6408A/TCAL9538 target attached, so init() can't reach
 * ALP_OK here (reg_read fails past the address check, same as every
 * other post-init-transfer case in this file -- e.g.
 * test_pca9451a_post_init_calls_reject_uninitialised); what this test
 * pins is that the address itself is NOT rejected as ALP_ERR_INVAL,
 * i.e. it clears the strap-range guard this fix widens. Before the
 * fix, tcal9538_init() returns ALP_ERR_INVAL for 0x20/0x21 before
 * ever reaching the bus -- that's exactly what regresses. */
ZTEST(alp_chips, test_tcal9538_init_accepts_tca6408a_alt_strap)
{
	tcal9538_t ctx;
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	zassert_not_equal(tcal9538_init(&ctx, bus, TCAL9538_I2C_ADDR_ALT_BASE),
	                  ALP_ERR_INVAL,
	                  "0x20 (TCA6408A alt-strap, A0=0) must not be rejected as an invalid address");
	zassert_not_equal(tcal9538_init(&ctx, bus, (uint8_t)(TCAL9538_I2C_ADDR_ALT_BASE + 1u)),
	                  ALP_ERR_INVAL,
	                  "0x21 (TCA6408A alt-strap, A0=1) must not be rejected as an invalid address");

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_tcal9538_calls_reject_uninitialised)
{
	tcal9538_t ctx = { 0 };
	bool       level;
	uint8_t    bits;

	zassert_equal(tcal9538_set_direction(&ctx, 0u, TCAL9538_DIR_OUTPUT), ALP_ERR_NOT_READY);
	zassert_equal(tcal9538_set_directions(&ctx, 0xFFu, 0x00u), ALP_ERR_NOT_READY);
	zassert_equal(tcal9538_set(&ctx, 0u, true), ALP_ERR_NOT_READY);
	zassert_equal(tcal9538_get(&ctx, 0u, &level), ALP_ERR_NOT_READY);
	zassert_equal(tcal9538_read_all(&ctx, &bits), ALP_ERR_NOT_READY);
	zassert_equal(tcal9538_write_all(&ctx, 0u), ALP_ERR_NOT_READY);

	tcal9538_deinit(&ctx);
	tcal9538_deinit(NULL);
}

ZTEST(alp_chips, test_tcal9538_pin_index_validation)
{
	/* Force .initialised so the function reaches the pin-index
     * check.  The chip has 8 pins (0..7); 8+ is invalid. */
	tcal9538_t ctx = { .initialised = true };

	zassert_equal(tcal9538_set_direction(&ctx, 8u, TCAL9538_DIR_OUTPUT), ALP_ERR_INVAL);
	zassert_equal(tcal9538_set(&ctx, 99u, true), ALP_ERR_INVAL);
	bool level;
	zassert_equal(tcal9538_get(&ctx, 99u, &level), ALP_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* tcal9538 -- fake-backed register-protocol tests                    */
/* ------------------------------------------------------------------ */
/* fake_tcal9538.c wires TWO instances: 0x73 (TCAL9538 strap, Agile IO
 * block present -> has_latched_irq = true) and 0x20 (TCA6408A
 * alt-strap, no Agile IO block -> has_latched_irq = false). */

static alp_i2c_t *open_tcal9538_bus(void)
{
	return alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
}

ZTEST(alp_chips, test_fake_tcal9538_set_input_latch_writes_reg42)
{
	fake_tcal9538_reset(TCAL9538_I2C_ADDR_BASE + 3u);
	alp_i2c_t *bus = open_tcal9538_bus();
	zassert_not_null(bus);

	tcal9538_t ctx;
	zassert_equal(tcal9538_init(&ctx, bus, TCAL9538_I2C_ADDR_BASE + 3u), ALP_OK);

	zassert_equal(tcal9538_set_input_latch(&ctx, 0xF0u), ALP_OK);
	zassert_equal(fake_tcal9538_get_reg(TCAL9538_I2C_ADDR_BASE + 3u, 0x42u), 0xF0u);
	zassert_equal(fake_tcal9538_write_count(TCAL9538_I2C_ADDR_BASE + 3u, 0x42u), 1u);

	tcal9538_deinit(&ctx);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_tcal9538_set_interrupt_mask_writes_reg45)
{
	fake_tcal9538_reset(TCAL9538_I2C_ADDR_BASE + 3u);
	alp_i2c_t *bus = open_tcal9538_bus();
	zassert_not_null(bus);

	tcal9538_t ctx;
	zassert_equal(tcal9538_init(&ctx, bus, TCAL9538_I2C_ADDR_BASE + 3u), ALP_OK);

	zassert_equal(tcal9538_set_interrupt_mask(&ctx, 0x0Fu), ALP_OK);
	zassert_equal(fake_tcal9538_get_reg(TCAL9538_I2C_ADDR_BASE + 3u, 0x45u), 0x0Fu);
	zassert_equal(fake_tcal9538_write_count(TCAL9538_I2C_ADDR_BASE + 3u, 0x45u), 1u);

	tcal9538_deinit(&ctx);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_tcal9538_get_interrupt_status_does_not_auto_clear)
{
	fake_tcal9538_reset(TCAL9538_I2C_ADDR_BASE + 3u);
	alp_i2c_t *bus = open_tcal9538_bus();
	zassert_not_null(bus);

	tcal9538_t ctx;
	zassert_equal(tcal9538_init(&ctx, bus, TCAL9538_I2C_ADDR_BASE + 3u), ALP_OK);

	fake_tcal9538_set_reg(TCAL9538_I2C_ADDR_BASE + 3u, 0x46u, 0xABu);

	uint8_t status = 0;
	zassert_equal(tcal9538_get_interrupt_status(&ctx, &status), ALP_OK);
	zassert_equal(status, 0xABu);

	/* Exactly one read of 0x46, and no follow-up read of the input
	 * port (0x00) -- a driver that "helpfully" auto-clears status by
	 * also reading 0x00 breaks a caller relying on it persisting. */
	zassert_equal(fake_tcal9538_read_count(TCAL9538_I2C_ADDR_BASE + 3u, 0x46u), 1u);
	zassert_equal(fake_tcal9538_read_count(TCAL9538_I2C_ADDR_BASE + 3u, 0x00u), 0u);

	tcal9538_deinit(&ctx);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_tcal9538_set_pull_bit_order)
{
	const uint8_t addr = TCAL9538_I2C_ADDR_BASE + 3u;
	alp_i2c_t    *bus  = open_tcal9538_bus();
	zassert_not_null(bus);

	/* UP: EN gets pin 4's bit set (0x10); SEL's POR default is 0xFF
	 * (datasheet default, see fake_tcal9538.c) so selecting "up" for a
	 * bit already 1 there writes 0x44 back UNCHANGED at 0xFF -- this
	 * is what pins the EN/SEL bit-order down: a swapped pair would
	 * write a different pattern to one of the two registers. */
	fake_tcal9538_reset(addr);
	tcal9538_t ctx;
	zassert_equal(tcal9538_init(&ctx, bus, addr), ALP_OK);
	zassert_equal(tcal9538_set_pull(&ctx, 4u, TCAL9538_PULL_UP), ALP_OK);
	zassert_equal(fake_tcal9538_get_reg(addr, 0x43u), 0x10u);
	zassert_equal(fake_tcal9538_get_reg(addr, 0x44u), 0xFFu);
	tcal9538_deinit(&ctx);

	/* DOWN: EN still gets bit 4 set (0x10); SEL clears bit 4 out of
	 * its 0xFF default -> 0xEF. */
	fake_tcal9538_reset(addr);
	zassert_equal(tcal9538_init(&ctx, bus, addr), ALP_OK);
	zassert_equal(tcal9538_set_pull(&ctx, 4u, TCAL9538_PULL_DOWN), ALP_OK);
	zassert_equal(fake_tcal9538_get_reg(addr, 0x43u), 0x10u);
	zassert_equal(fake_tcal9538_get_reg(addr, 0x44u), 0xEFu);
	tcal9538_deinit(&ctx);

	/* NONE: EN clears bit 4 -> 0x00.  The driver always issues a
	 * read-modify-write on BOTH registers (it never conditionally
	 * skips the 0x44 transaction), so SEL is still written here -- but
	 * with the SAME value it already held (0xFF), i.e. NONE never
	 * changes which pins are pull-up-vs-pull-down-selected, only
	 * whether the pull is enabled at all. */
	fake_tcal9538_reset(addr);
	zassert_equal(tcal9538_init(&ctx, bus, addr), ALP_OK);
	zassert_equal(tcal9538_set_pull(&ctx, 4u, TCAL9538_PULL_NONE), ALP_OK);
	zassert_equal(fake_tcal9538_get_reg(addr, 0x43u), 0x00u);
	zassert_equal(fake_tcal9538_get_reg(addr, 0x44u), 0xFFu, "SEL value must be left unchanged");
	tcal9538_deinit(&ctx);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_tcal9538_alt_part_nosupport_issues_zero_bus_traffic)
{
	fake_tcal9538_reset(TCAL9538_I2C_ADDR_ALT_BASE);
	alp_i2c_t *bus = open_tcal9538_bus();
	zassert_not_null(bus);

	tcal9538_t ctx;
	zassert_equal(tcal9538_init(&ctx, bus, TCAL9538_I2C_ADDR_ALT_BASE), ALP_OK);

	uint32_t baseline = fake_tcal9538_total_transactions(TCAL9538_I2C_ADDR_ALT_BASE);

	uint8_t status;
	bool    level;
	zassert_equal(tcal9538_set_input_latch(&ctx, 0xFFu), ALP_ERR_NOSUPPORT);
	zassert_equal(tcal9538_set_interrupt_mask(&ctx, 0xFFu), ALP_ERR_NOSUPPORT);
	zassert_equal(tcal9538_get_interrupt_status(&ctx, &status), ALP_ERR_NOSUPPORT);
	zassert_equal(tcal9538_set_pull(&ctx, 0u, TCAL9538_PULL_UP), ALP_ERR_NOSUPPORT);
	(void)level;

	/* Zero NEW bus transactions -- catches a NOSUPPORT guard placed
	 * after the bus write instead of before it. */
	zassert_equal(fake_tcal9538_total_transactions(TCAL9538_I2C_ADDR_ALT_BASE), baseline);

	tcal9538_deinit(&ctx);
	alp_i2c_close(bus);
}

/* Proves tcal9538_set_polarity_inversion() lands its byte in register
 * 0x02 specifically, not the adjacent output-port register (0x01) or
 * configuration register (0x03) -- a one-register-off mistake here
 * would drive an EVK output pin instead of just flipping a read-back
 * bit, exactly the class of bug this accessor exists to make
 * impossible to make by hand in the example. Mutation-verified:
 * changing TCAL9538_REG_POL from 0x02u to 0x01u in tcal9538.c reddens
 * this test (reg 0x02 stays 0x00, the zassert_equal on it fails). */
ZTEST(alp_chips, test_fake_tcal9538_set_polarity_inversion_writes_reg02_only)
{
	fake_tcal9538_reset(TCAL9538_I2C_ADDR_BASE + 3u);
	alp_i2c_t *bus = open_tcal9538_bus();
	zassert_not_null(bus);

	tcal9538_t ctx;
	zassert_equal(tcal9538_init(&ctx, bus, TCAL9538_I2C_ADDR_BASE + 3u), ALP_OK);

	zassert_equal(tcal9538_set_polarity_inversion(&ctx, 0xFFu), ALP_OK);
	zassert_equal(fake_tcal9538_get_reg(TCAL9538_I2C_ADDR_BASE + 3u, 0x02u), 0xFFu);
	zassert_equal(fake_tcal9538_write_count(TCAL9538_I2C_ADDR_BASE + 3u, 0x02u), 1u);
	/* The two neighbouring registers must be untouched. */
	zassert_equal(fake_tcal9538_write_count(TCAL9538_I2C_ADDR_BASE + 3u, 0x01u), 0u);
	zassert_equal(fake_tcal9538_write_count(TCAL9538_I2C_ADDR_BASE + 3u, 0x03u), 0u);

	zassert_equal(tcal9538_set_polarity_inversion(&ctx, 0x00u), ALP_OK);
	zassert_equal(fake_tcal9538_get_reg(TCAL9538_I2C_ADDR_BASE + 3u, 0x02u), 0x00u);

	tcal9538_deinit(&ctx);
	alp_i2c_close(bus);
}

/* Polarity inversion is a base register (0x00..0x03), shared with the
 * TCA6408A/PCA9538 alt-population -- unlike the Agile-IO block, it
 * must NOT return ALP_ERR_NOSUPPORT on the alt-strap address. */
ZTEST(alp_chips, test_fake_tcal9538_set_polarity_inversion_works_on_alt_part)
{
	fake_tcal9538_reset(TCAL9538_I2C_ADDR_ALT_BASE);
	alp_i2c_t *bus = open_tcal9538_bus();
	zassert_not_null(bus);

	tcal9538_t ctx;
	zassert_equal(tcal9538_init(&ctx, bus, TCAL9538_I2C_ADDR_ALT_BASE), ALP_OK);

	zassert_equal(tcal9538_set_polarity_inversion(&ctx, 0xFFu), ALP_OK);
	zassert_equal(fake_tcal9538_get_reg(TCAL9538_I2C_ADDR_ALT_BASE, 0x02u), 0xFFu);

	tcal9538_deinit(&ctx);
	alp_i2c_close(bus);
}
