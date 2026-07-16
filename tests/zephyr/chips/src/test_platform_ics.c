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
 * TCA6408A alt-population, EVK_I2C_ADDR_TCA6408A_MAIN); 0 is the
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
 * TCA6408A alt-population (EVK_I2C_ADDR_TCA6408A_MAIN = 0x20, bench-
 * confirmed) -- the exact case examples/peripheral-io/i2c-device-hub
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
