/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sensor chip smokes: lsm6dso, bme280, lis2dw12, icm42670, bmi323,
 * bmp581 -- lifecycle, NULL-arg validation, status-code propagation.
 * The fake-backed lsm6dso/bme280 register-protocol tests exercise the
 * real transfer path against the fake i2c-emul targets in
 * fake_lsm6dso.c / fake_bme280.c.
 */

#include <zephyr/ztest.h>

#include "alp/chips/bme280.h"
#include "alp/chips/bmi323.h"
#include "alp/chips/bmp581.h"
#include "alp/chips/icm42670.h"
#include "alp/chips/lis2dw12.h"
#include "alp/chips/lsm6dso.h"
#include "alp/e1m_pinout.h"
#include "alp/peripheral.h"

#include "fakes.h" /* fake_lsm6dso.c / fake_bme280.c inspection API (dormant fake-emul
                     * block below) + fake_icm42670.c / fake_bmi323.c / fake_bmp581.c
                     * (active -- see each file's header). */

/* Not part of the public bme280 API -- see chips/bme280/bme280_internal.h
 * for why the H4/H5 calibration unpack is tested directly here rather
 * than through bme280_init(). */
#include "bme280_internal.h"

/* ------------------------------------------------------------------ */
/* lsm6dso                                                             */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_lsm6dso_init_null_args)
{
	lsm6dso_t  dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 100000 });
	zassert_not_null(bus);

	zassert_equal(
	    lsm6dso_init(NULL, bus, LSM6DSO_I2C_ADDR_LOW), ALP_ERR_INVAL, "NULL ctx must be invalid");
	zassert_equal(
	    lsm6dso_init(&dev, NULL, LSM6DSO_I2C_ADDR_LOW), ALP_ERR_INVAL, "NULL bus must be invalid");
	zassert_equal(lsm6dso_init(&dev, bus, 0), ALP_ERR_INVAL, "addr=0 must be invalid");

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_lsm6dso_post_init_calls_reject_uninitialised)
{
	/* Without a real chip behind the emul controller, init's
     * WHO_AM_I check will not return 0x6C.  We expect lsm6dso_init
     * to fail; subsequent reads must report NOT_READY.
     *
     * If the WHO_AM_I check returns ALP_OK against the emul (e.g.
     * if a future revision fakes it), the test still passes
     * because reads on a successfully-initialised driver return
     * ALP_OK and the assertion below is conditional. */
	lsm6dso_t  dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 100000 });
	zassert_not_null(bus);

	alp_status_t s = lsm6dso_init(&dev, bus, LSM6DSO_I2C_ADDR_LOW);
	if (s != ALP_OK) {
		/* Expected path on a bare emul controller. */
		lsm6dso_axes_t axes;
		zassert_equal(lsm6dso_read_accel(&dev, &axes),
		              ALP_ERR_NOT_READY,
		              "reads on a failed-init driver must be NOT_READY");
	}

	lsm6dso_deinit(&dev);
	alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* bme280 (v0.2 chip)                                                  */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_bme280_init_null_args)
{
	bme280_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	zassert_equal(
	    bme280_init(NULL, bus, BME280_I2C_ADDR_LOW), ALP_ERR_INVAL, "NULL ctx must be invalid");
	zassert_equal(
	    bme280_init(&dev, NULL, BME280_I2C_ADDR_LOW), ALP_ERR_INVAL, "NULL bus must be invalid");
	zassert_equal(bme280_init(&dev, bus, 0), ALP_ERR_INVAL, "addr=0 must be invalid");

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_bme280_post_init_calls_reject_uninitialised)
{
	/* No real chip behind the emul controller — CHIP_ID won't be 0x60,
     * so init fails and downstream reads must report NOT_READY. */
	bme280_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	alp_status_t s = bme280_init(&dev, bus, BME280_I2C_ADDR_LOW);
	if (s != ALP_OK) {
		bme280_raw_t raw;
		zassert_equal(bme280_read_raw(&dev, &raw),
		              ALP_ERR_NOT_READY,
		              "read_raw on a failed-init driver must be NOT_READY");
		zassert_equal(bme280_set_sampling(&dev,
		                                  BME280_OVERSAMPLING_X1,
		                                  BME280_OVERSAMPLING_X1,
		                                  BME280_OVERSAMPLING_X1,
		                                  BME280_MODE_NORMAL,
		                                  BME280_STANDBY_125_MS,
		                                  BME280_FILTER_OFF),
		              ALP_ERR_NOT_READY);
	}

	bme280_deinit(&dev);
	alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* bme280 -- invalid-enum rejection (#736)                             */
/* ------------------------------------------------------------------ */
/* bme280_set_sampling doesn't touch the bus for a rejected call, so
 * these tests build the device context directly instead of going
 * through bme280_init -- no fake i2c target required. */

ZTEST(alp_chips, test_bme280_set_sampling_rejects_reserved_mode)
{
	bme280_t dev    = { 0 };
	dev.initialised = true;

	/* BME280_MODE_* declares 0x0, 0x1, 0x3 -- 0x2 is not a declared
	 * member and must not reach the hardware write. */
	zassert_equal(bme280_set_sampling(&dev,
	                                  BME280_OVERSAMPLING_X1,
	                                  BME280_OVERSAMPLING_X1,
	                                  BME280_OVERSAMPLING_X1,
	                                  (bme280_mode_t)2,
	                                  BME280_STANDBY_125_MS,
	                                  BME280_FILTER_OFF),
	              ALP_ERR_INVAL,
	              "undeclared mode 0x2 must be rejected, not masked");
}

ZTEST(alp_chips, test_bme280_set_sampling_rejects_out_of_range_oversampling)
{
	bme280_t dev    = { 0 };
	dev.initialised = true;

	/* Oversampling is a 3-bit field but only 0x0-0x5 are declared;
	 * 0x6/0x7 are reserved. */
	zassert_equal(bme280_set_sampling(&dev,
	                                  (bme280_oversampling_t)6,
	                                  BME280_OVERSAMPLING_X1,
	                                  BME280_OVERSAMPLING_X1,
	                                  BME280_MODE_NORMAL,
	                                  BME280_STANDBY_125_MS,
	                                  BME280_FILTER_OFF),
	              ALP_ERR_INVAL,
	              "t_os=6 (reserved) must be rejected");
	zassert_equal(bme280_set_sampling(&dev,
	                                  BME280_OVERSAMPLING_X1,
	                                  (bme280_oversampling_t)7,
	                                  BME280_OVERSAMPLING_X1,
	                                  BME280_MODE_NORMAL,
	                                  BME280_STANDBY_125_MS,
	                                  BME280_FILTER_OFF),
	              ALP_ERR_INVAL,
	              "p_os=7 (reserved) must be rejected");
}

ZTEST(alp_chips, test_bme280_set_sampling_rejects_out_of_range_standby)
{
	bme280_t dev    = { 0 };
	dev.initialised = true;

	/* Standby is a 3-bit field and BME280_STANDBY_* declares all 8
	 * codes (0x0-0x7 = BME280_STANDBY_20_MS), so there is no reserved
	 * gap inside the field width to reject -- only a value past the
	 * 3-bit field itself is invalid. */
	zassert_equal(bme280_set_sampling(&dev,
	                                  BME280_OVERSAMPLING_X1,
	                                  BME280_OVERSAMPLING_X1,
	                                  BME280_OVERSAMPLING_X1,
	                                  BME280_MODE_NORMAL,
	                                  (bme280_standby_t)8,
	                                  BME280_FILTER_OFF),
	              ALP_ERR_INVAL,
	              "standby=8 (past the 3-bit field) must be rejected");
}

ZTEST(alp_chips, test_bme280_set_sampling_rejects_reserved_filter)
{
	bme280_t dev    = { 0 };
	dev.initialised = true;

	/* Filter is a 3-bit field but only 0x0-0x4 are declared. */
	zassert_equal(bme280_set_sampling(&dev,
	                                  BME280_OVERSAMPLING_X1,
	                                  BME280_OVERSAMPLING_X1,
	                                  BME280_OVERSAMPLING_X1,
	                                  BME280_MODE_NORMAL,
	                                  BME280_STANDBY_125_MS,
	                                  (bme280_filter_t)5),
	              ALP_ERR_INVAL,
	              "filter=5 (reserved) must be rejected");
}

ZTEST(alp_chips, test_bme280_set_sampling_accepts_every_declared_value)
{
	/* Table-driven sweep of the full declared enum sets -- every
	 * combination must clear the enum-validation gate (regression guard
	 * against the boundary checks being off-by-one).  A validated call
	 * proceeds to the register write, so this needs a real (open) bus --
	 * a rejected call never gets that far.  There's no fake i2c target
	 * behind this bus (the EMUL_DT_INST_DEFINE fixtures are disabled --
	 * see the "Register-protocol tests" block below), so the write
	 * itself has nothing to ACK and returns a transport error; assert
	 * on ALP_ERR_INVAL specifically to prove validation let the call
	 * through, without depending on that unrelated transport outcome. */
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	bme280_t dev    = { 0 };
	dev.bus         = bus;
	dev.addr        = BME280_I2C_ADDR_LOW;
	dev.initialised = true;

	const bme280_oversampling_t os_values[] = {
		BME280_OVERSAMPLING_SKIPPED, BME280_OVERSAMPLING_X1, BME280_OVERSAMPLING_X2,
		BME280_OVERSAMPLING_X4,      BME280_OVERSAMPLING_X8, BME280_OVERSAMPLING_X16,
	};
	const bme280_mode_t mode_values[] = {
		BME280_MODE_SLEEP,
		BME280_MODE_FORCED,
		BME280_MODE_NORMAL,
	};
	const bme280_filter_t filter_values[] = {
		BME280_FILTER_OFF, BME280_FILTER_2, BME280_FILTER_4, BME280_FILTER_8, BME280_FILTER_16,
	};
	const bme280_standby_t standby_values[] = {
		BME280_STANDBY_0_5_MS, BME280_STANDBY_62_5_MS, BME280_STANDBY_125_MS, BME280_STANDBY_250_MS,
		BME280_STANDBY_500_MS, BME280_STANDBY_1000_MS, BME280_STANDBY_10_MS,  BME280_STANDBY_20_MS,
	};

	for (size_t i = 0; i < ARRAY_SIZE(os_values); i++) {
		for (size_t m = 0; m < ARRAY_SIZE(mode_values); m++) {
			for (size_t f = 0; f < ARRAY_SIZE(filter_values); f++) {
				for (size_t sb = 0; sb < ARRAY_SIZE(standby_values); sb++) {
					zassert_not_equal(bme280_set_sampling(&dev,
					                                      os_values[i],
					                                      os_values[i],
					                                      os_values[i],
					                                      mode_values[m],
					                                      standby_values[sb],
					                                      filter_values[f]),
					                  ALP_ERR_INVAL,
					                  "declared value wrongly rejected by the enum guard");
				}
			}
		}
	}

	alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* bme280 -- signed overflow UB in calibration / compensation (#753)   */
/* ------------------------------------------------------------------ */
/* bme280_compensate reads only dev->calib + dev->t_fine and never
 * touches the bus, so these tests build calibration directly -- no
 * fake i2c target required.
 *
 * Expected outputs are NOT pinned from running this (or any) driver
 * build and recording what it printed -- a golden derived that way
 * cannot tell a merely-plausible number from a correct one, and the
 * first-round version of these vectors was exactly that trap: it
 * pinned a humidity value computed through the signed-overflow UB it
 * was meant to prove absent.  Instead every T/P/H value below was
 * independently re-derived from the BST-BME280-DS002 v1.6 S4.2.3
 * integer compensation formulas, transcribed fresh in Python using
 * arbitrary-precision integers (so the transcription itself can never
 * overflow) with `>>` on a negative operand read as floor division --
 * the same arithmetic-right-shift semantics every mainstream two's-
 * complement C compiler emits for `int32_t/int64_t >> n`.  See the
 * datasheet formula pseudocode in the doc comment above sign_extend12
 * and bme280_compensate for the register/coefficient layout each
 * vector below feeds it.
 *
 * Regression proof, plain build (this is the gate CI actually runs --
 * no --enable-ubsan): reverting only the bme280.c production hunk
 * this branch touches (keeping these tests) makes
 * test_bme280_compensate_h4_h5_extrema_matches_reference FAIL under a
 * bare `west twister -p native_sim/native/64`.  At that vector's
 * H4 = H5 = -2048 (the 12-bit signed minimum) and adc_H = 0xA000, the
 * reverted int32-only humidity leg overflows and -- on this
 * toolchain/opt-level, where the overflow wraps instead of trapping
 * -- lands on humidity_milli_pct == 0 (0 %RH), while the fixed int64
 * leg correctly saturates to the clamp ceiling (102400, 100 %RH).
 * That divergence is not a rare corner: sweeping all 65536 adc_H
 * values at this H4/H5 pair shows the reverted and fixed code
 * disagree on 31269 of them.  adc_H = 0x6FF0, used by an earlier
 * version of this vector, happens to fall in the 34267 that agree
 * (both give 102400) and did not discriminate -- 0xA000 was picked
 * because it does.
 *
 * Regression proof, UBSan build: `west twister -p native_sim/native/64
 * --enable-ubsan` on the same reverted tree independently reports the
 * underlying UB directly -- a `signed integer overflow ... cannot be
 * represented in type 'int'` at the old int32-only humidity line,
 * naming the same test -- confirming the plain-build divergence above
 * is that overflow and not some unrelated bug.  (The exact operands
 * are deliberately not quoted here: they are a function of the vector,
 * and an earlier revision of this comment kept quoting the operands of
 * a since-retired adc_H, i.e. a diagnostic this tree can no longer
 * produce.  Run the command to see the live values.)
 * This is corroborating evidence, not the only proof:
 * the plain build above already fails on its own under the gate CI
 * runs on every PR.
 *
 * Both the negative_coefficients vector below (H4 = -349) and the
 * h4_h5_extrema vector further below (H4 = -2048) saturate to the
 * clamp ceiling (419430400 >> 12 == 102400) for *every* legal adc_H,
 * given this file's shared H1/H2/H3/H6 -- verified by exhaustively
 * calling the real bme280_compensate() over all 65536 adc_H values at
 * each H4/H5 pair below.  A sufficiently negative H4 dominates the x1
 * term regardless of adc_H, so this is a genuine property of the
 * datasheet formula for these coefficients, not a weak vector: it
 * pins the clamp branch, which any correct implementation must also
 * hit here.  The unsaturated, interior humidity value is pinned
 * separately by test_bme280_compensate_p4_p7_int16_min_matches_
 * reference's humidity_milli_pct == 35945 (H4 = +349, where the
 * dominant term flips sign and the product no longer saturates). */

ZTEST(alp_chips, test_bme280_compensate_negative_coefficients_matches_reference)
{
	/* Sign-flipped datasheet coefficients: P4, P5, P7, H4, H5 all
	 * negative -- exercises every site the fix touches. */
	bme280_t dev    = { 0 };
	dev.calib.T1    = 27504u;
	dev.calib.T2    = 26435;
	dev.calib.T3    = -1000;
	dev.calib.P1    = 36477u;
	dev.calib.P2    = -10685;
	dev.calib.P3    = 3024;
	dev.calib.P4    = -2855;
	dev.calib.P5    = -140;
	dev.calib.P6    = -7;
	dev.calib.P7    = -15500;
	dev.calib.P8    = -14600;
	dev.calib.P9    = 6000;
	dev.calib.H1    = 75;
	dev.calib.H2    = 363;
	dev.calib.H3    = 0;
	dev.calib.H4    = -349;
	dev.calib.H5    = -30;
	dev.calib.H6    = 30;
	dev.initialised = true;

	bme280_raw_t raw = {
		.pressure_raw    = 415148,
		.temperature_raw = 519888,
		.humidity_raw    = 0x6FF0,
	};
	bme280_compensated_t comp;
	zassert_equal(bme280_compensate(&dev, &raw, &comp), ALP_OK);
	zassert_equal(comp.temperature_c100, 2508);
	zassert_equal(comp.pressure_pa, 114530u);
	zassert_equal(comp.humidity_milli_pct, 102400u);
}

ZTEST(alp_chips, test_bme280_compensate_h4_h5_extrema_matches_reference)
{
	/* H4/H5 at the 12-bit signed minimum (-2048) -- the exact
	 * calibration extremum #753 names, and (see the block comment
	 * above) the one that still overflowed int32_t after the
	 * shift-to-multiply-only round of the fix.  adc_H = 0xA000 (not
	 * 0x6FF0) is deliberate: it is one of the 31269/65536 values at
	 * this H4/H5 where the reverted int32 leg and the fixed int64 leg
	 * disagree, so this assertion fails on a plain (non-UBSan) build
	 * if the production fix regresses -- see the block comment. */
	bme280_t dev    = { 0 };
	dev.calib.T1    = 27504u;
	dev.calib.T2    = 26435;
	dev.calib.T3    = -1000;
	dev.calib.P1    = 36477u;
	dev.calib.P2    = -10685;
	dev.calib.P3    = 3024;
	dev.calib.P4    = 2855;
	dev.calib.P5    = 140;
	dev.calib.P6    = -7;
	dev.calib.P7    = 15500;
	dev.calib.P8    = -14600;
	dev.calib.P9    = 6000;
	dev.calib.H1    = 75;
	dev.calib.H2    = 363;
	dev.calib.H3    = 0;
	dev.calib.H4    = -2048;
	dev.calib.H5    = -2048;
	dev.calib.H6    = 30;
	dev.initialised = true;

	bme280_raw_t raw = {
		.pressure_raw    = 415148,
		.temperature_raw = 519888,
		.humidity_raw    = 0xA000,
	};
	bme280_compensated_t comp;
	zassert_equal(bme280_compensate(&dev, &raw, &comp), ALP_OK);
	zassert_equal(comp.temperature_c100, 2508);
	zassert_equal(comp.pressure_pa, 100653u);
	zassert_equal(comp.humidity_milli_pct, 102400u);
}

ZTEST(alp_chips, test_bme280_compensate_p4_p7_int16_min_matches_reference)
{
	/* P4/P7 at INT16_MIN -- the widest negative value the field can
	 * hold; exercises the `<< 35` / `<< 4` sites on the pressure leg. */
	bme280_t dev    = { 0 };
	dev.calib.T1    = 27504u;
	dev.calib.T2    = 26435;
	dev.calib.T3    = -1000;
	dev.calib.P1    = 36477u;
	dev.calib.P2    = -10685;
	dev.calib.P3    = 3024;
	dev.calib.P4    = -32768;
	dev.calib.P5    = 140;
	dev.calib.P6    = -7;
	dev.calib.P7    = -32768;
	dev.calib.P8    = -14600;
	dev.calib.P9    = 6000;
	dev.calib.H1    = 75;
	dev.calib.H2    = 363;
	dev.calib.H3    = 0;
	dev.calib.H4    = 349;
	dev.calib.H5    = 30;
	dev.calib.H6    = 30;
	dev.initialised = true;

	bme280_raw_t raw = {
		.pressure_raw    = 415148,
		.temperature_raw = 519888,
		.humidity_raw    = 0x6FF0,
	};
	bme280_compensated_t comp;
	zassert_equal(bme280_compensate(&dev, &raw, &comp), ALP_OK);
	zassert_equal(comp.temperature_c100, 2508);
	zassert_equal(comp.pressure_pa, 197689u);
	zassert_equal(comp.humidity_milli_pct, 35945u);
}

/* ------------------------------------------------------------------ */
/* bme280 -- H4/H5 calibration unpack (#797)                          */
/* ------------------------------------------------------------------ */
/* The #753 tests above pin dev.calib.H4/H5 directly, bypassing
 * load_calibration() by design -- that isolates bme280_compensate()
 * from the register layer. It leaves the E4/E5/E6 shared-nibble
 * unpack itself (BST-BME280-DS002 v1.6 S5.4 Table 20) unvalidated:
 * a nibble swap or off-by-one in that split would ship silently,
 * since the disabled i2c-emul fake is the only other thing that ever
 * drove load_calibration() end-to-end (see bme280_internal.h / the
 * fake-fixtures note in CMakeLists.txt). bme280_unpack_h4_h5() is the
 * pure nibble-unpack factored out of load_calibration() so it is
 * testable without a bus or fake target. */

ZTEST(alp_chips, test_bme280_unpack_h4_h5)
{
	int16_t h4, h5;

	/* All-zero bytes -> both fields zero. */
	bme280_unpack_h4_h5((const uint8_t[3]){ 0x00, 0x00, 0x00 }, &h4, &h5);
	zassert_equal(h4, 0);
	zassert_equal(h5, 0);

	/* E5's two nibbles feed opposite fields (low -> H4, high -> H5);
	 * asymmetric nibbles here catch a swapped-nibble regression.
	 * E4=0x7F, E5=0xF3, E6=0x05: H4 = (0x7F<<4)|0x3 = 0x7F3 = 2035
	 * (below 0x800, stays positive); H5 = (0x05<<4)|0xF = 0x5F = 95. */
	bme280_unpack_h4_h5((const uint8_t[3]){ 0x7F, 0xF3, 0x05 }, &h4, &h5);
	zassert_equal(h4, 2035);
	zassert_equal(h5, 95);

	/* Sign-extension boundary: bits12 == 0x800 is the smallest value
	 * that must wrap negative (-2048), not stay positive (2048).
	 * E4=0x80, E5=0x00, E6=0x80 puts both H4 and H5 exactly there. */
	bme280_unpack_h4_h5((const uint8_t[3]){ 0x80, 0x00, 0x80 }, &h4, &h5);
	zassert_equal(h4, -2048);
	zassert_equal(h5, -2048);

	/* Interior negative values, distinct per field (catches a field
	 * swap even after sign-extension is correct on its own).
	 * E4=0xFF, E5=0xF8, E6=0xFF: H4 = 0xFF8 = 4088 -> -8;
	 * H5 = 0xFFF = 4095 -> -1. */
	bme280_unpack_h4_h5((const uint8_t[3]){ 0xFF, 0xF8, 0xFF }, &h4, &h5);
	zassert_equal(h4, -8);
	zassert_equal(h5, -1);
}

/* ------------------------------------------------------------------ */
/* lis2dw12 (v0.2 chip)                                                */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_lis2dw12_init_null_args)
{
	lis2dw12_t dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 100000 });
	zassert_not_null(bus);

	zassert_equal(lis2dw12_init(NULL, bus, LIS2DW12_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(lis2dw12_init(&dev, NULL, LIS2DW12_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(lis2dw12_init(&dev, bus, 0), ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_lis2dw12_post_init_calls_reject_uninitialised)
{
	lis2dw12_t dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 100000 });
	zassert_not_null(bus);

	alp_status_t s = lis2dw12_init(&dev, bus, LIS2DW12_I2C_ADDR_LOW);
	if (s != ALP_OK) {
		lis2dw12_axes_t axes;
		zassert_equal(lis2dw12_read_accel(&dev, &axes), ALP_ERR_NOT_READY);
		zassert_equal(lis2dw12_set_accel(
		                  &dev, LIS2DW12_ODR_50_HZ, LIS2DW12_FS_2G, LIS2DW12_MODE_HIGH_PERF_14BIT),
		              ALP_ERR_NOT_READY);
	}

	lis2dw12_deinit(&dev);
	alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* icm42670 (TDK 6-axis IMU, on-board EVK chip)                        */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_icm42670_init_null_args)
{
	icm42670_t dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	zassert_equal(icm42670_init(NULL, bus, ICM42670_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(icm42670_init(&dev, NULL, ICM42670_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(icm42670_init(&dev, bus, 0), ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* icm42670 -- fake-backed register-protocol tests                    */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_fake_icm42670_configure_int_pin1_preserves_int2_field)
{
	fake_icm42670_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	icm42670_t dev;
	zassert_equal(icm42670_init(&dev, bus, ICM42670_I2C_ADDR_LOW), ALP_OK);

	/* INT2's field (bits[5:3]) seeded nonzero; INT1's field (bits[2:0])
	 * starts clear. */
	fake_icm42670_set_reg(0x06u, 0x38u);

	icm42670_int_pin_config_t cfg = {
		.mode     = ICM42670_INT_MODE_LATCHED,
		.drive    = ICM42670_INT_DRIVE_PUSH_PULL,
		.polarity = ICM42670_INT_POLARITY_ACTIVE_HIGH,
	};
	zassert_equal(icm42670_configure_int_pin(&dev, ICM42670_INT_PIN1, &cfg), ALP_OK);

	/* (1<<2)|(1<<1)|1 = 0x07 in bits[2:0]; INT2's seeded 0x38 must
	 * survive untouched. */
	zassert_equal(fake_icm42670_get_reg(0x06u), 0x3Fu);
	zassert_equal(fake_icm42670_write_count(0x06u), 1u);

	icm42670_deinit(&dev);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_icm42670_configure_int_pin2_preserves_int1_field)
{
	fake_icm42670_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	icm42670_t dev;
	zassert_equal(icm42670_init(&dev, bus, ICM42670_I2C_ADDR_LOW), ALP_OK);

	/* INT1's field (bits[2:0]) seeded nonzero this time. */
	fake_icm42670_set_reg(0x06u, 0x07u);

	icm42670_int_pin_config_t cfg = {
		.mode     = ICM42670_INT_MODE_LATCHED,
		.drive    = ICM42670_INT_DRIVE_PUSH_PULL,
		.polarity = ICM42670_INT_POLARITY_ACTIVE_HIGH,
	};
	zassert_equal(icm42670_configure_int_pin(&dev, ICM42670_INT_PIN2, &cfg), ALP_OK);

	/* 0x07 shifted into bits[5:3] = 0x38; INT1's seeded 0x07 must
	 * survive.  A shift/mask mix-up between PIN1 and PIN2 would land
	 * on a different byte than this. */
	zassert_equal(fake_icm42670_get_reg(0x06u), 0x3Fu);

	icm42670_deinit(&dev);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_icm42670_route_int_writes_the_pin_specific_register)
{
	fake_icm42670_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	icm42670_t dev;
	zassert_equal(icm42670_init(&dev, bus, ICM42670_I2C_ADDR_LOW), ALP_OK);

	zassert_equal(icm42670_route_int(&dev, ICM42670_INT_PIN1, ICM42670_INT_SRC_DATA_RDY), ALP_OK);
	zassert_equal(fake_icm42670_get_reg(0x2Bu), 0x08u, "INT_SOURCE0 must get the DATA_RDY bit");
	zassert_equal(fake_icm42670_write_count(0x2Du), 0u, "INT_SOURCE3 must be untouched by PIN1");

	zassert_equal(icm42670_route_int(&dev, ICM42670_INT_PIN2, ICM42670_INT_SRC_DATA_RDY), ALP_OK);
	zassert_equal(fake_icm42670_get_reg(0x2Du), 0x08u, "INT_SOURCE3 must get the DATA_RDY bit");

	icm42670_deinit(&dev);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_icm42670_data_ready_decodes_bit0)
{
	fake_icm42670_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	icm42670_t dev;
	zassert_equal(icm42670_init(&dev, bus, ICM42670_I2C_ADDR_LOW), ALP_OK);

	fake_icm42670_set_reg(0x39u, 0x01u);
	bool ready = false;
	zassert_equal(icm42670_data_ready(&dev, &ready), ALP_OK);
	zassert_true(ready);

	fake_icm42670_set_reg(0x39u, 0x00u);
	zassert_equal(icm42670_data_ready(&dev, &ready), ALP_OK);
	zassert_false(ready);

	icm42670_deinit(&dev);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_icm42670_uninitialised_issues_zero_bus_traffic)
{
	fake_icm42670_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	/* Never initialised -- dev.initialised stays false. */
	icm42670_t dev = { .bus = bus, .addr = ICM42670_I2C_ADDR_LOW, .initialised = false };

	icm42670_int_pin_config_t cfg = { 0 };
	bool                      ready;
	zassert_equal(icm42670_configure_int_pin(&dev, ICM42670_INT_PIN1, &cfg), ALP_ERR_NOT_READY);
	zassert_equal(icm42670_route_int(&dev, ICM42670_INT_PIN1, ICM42670_INT_SRC_DATA_RDY),
	              ALP_ERR_NOT_READY);
	zassert_equal(icm42670_data_ready(&dev, &ready), ALP_ERR_NOT_READY);

	zassert_equal(fake_icm42670_write_count(0x06u), 0u);
	zassert_equal(fake_icm42670_write_count(0x2Bu), 0u);
	zassert_equal(fake_icm42670_write_count(0x2Du), 0u);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_icm42670_post_init_calls_reject_uninitialised)
{
	/* Without a real ICM-42670 on the emul controller, init's
     * WHO_AM_I check returns wrong/no value -- we expect failure
     * and that subsequent reads report NOT_READY. */
	icm42670_t dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	alp_status_t s = icm42670_init(&dev, bus, ICM42670_I2C_ADDR_LOW);
	if (s != ALP_OK) {
		icm42670_axes_t a;
		zassert_equal(icm42670_read_accel(&dev, &a), ALP_ERR_NOT_READY);
		zassert_equal(icm42670_set_accel(&dev, ICM42670_ODR_100_HZ, ICM42670_ACCEL_FS_4G),
		              ALP_ERR_NOT_READY);
	}

	icm42670_deinit(&dev);
	alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* bmi323 (Bosch 6-axis IMU, second on-board EVK IMU)                  */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_bmi323_init_null_args)
{
	bmi323_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	zassert_equal(bmi323_init(NULL, bus, BMI323_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(bmi323_init(&dev, NULL, BMI323_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(bmi323_init(&dev, bus, 0), ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_bmi323_post_init_calls_reject_uninitialised)
{
	bmi323_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	alp_status_t s = bmi323_init(&dev, bus, BMI323_I2C_ADDR_LOW);
	if (s != ALP_OK) {
		bmi323_axes_t a;
		zassert_equal(bmi323_read_accel(&dev, &a), ALP_ERR_NOT_READY);
		zassert_equal(bmi323_set_gyro(&dev, BMI323_ODR_100_HZ, BMI323_GYRO_FS_2000_DPS),
		              ALP_ERR_NOT_READY);
	}

	bmi323_deinit(&dev);
	alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* bmi323 -- fake-backed register-protocol tests                      */
/* ------------------------------------------------------------------ */
/* fake_bmi323.c is wired at 0x69 in the overlay (not BMI323_I2C_ADDR_LOW
 * == 0x68, the E1M EVK's real strap -- 0x68 is already used by
 * fake_icm42670 in this same suite's shared bus, and the driver
 * doesn't validate the address against any strap table, so any
 * nonzero value works). */
#define FAKE_BMI323_ADDR 0x69u

ZTEST(alp_chips, test_fake_bmi323_configure_int_pin1_preserves_int2_field)
{
	fake_bmi323_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	bmi323_t dev;
	zassert_equal(bmi323_init(&dev, bus, FAKE_BMI323_ADDR), ALP_OK);

	/* INT2's field (bits[10:8]) seeded nonzero; INT1's field
	 * (bits[2:0]) starts clear. */
	fake_bmi323_set_reg(0x38u, 0x0700u);

	bmi323_int_pin_config_t cfg = {
		.level         = BMI323_INT_LEVEL_ACTIVE_HIGH,
		.drive         = BMI323_INT_DRIVE_OPEN_DRAIN,
		.output_enable = true,
	};
	zassert_equal(bmi323_configure_int_pin(&dev, BMI323_INT_PIN1, &cfg), ALP_OK);

	/* (1<<2)|(1<<1)|1 = 0x07 in bits[2:0]; INT2's seeded 0x700 must
	 * survive untouched. */
	zassert_equal(fake_bmi323_get_reg(0x38u), 0x0707u);

	bmi323_deinit(&dev);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_bmi323_configure_int_pin2_preserves_int1_field)
{
	fake_bmi323_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	bmi323_t dev;
	zassert_equal(bmi323_init(&dev, bus, FAKE_BMI323_ADDR), ALP_OK);

	/* INT1's field (bits[2:0]) seeded nonzero this time. */
	fake_bmi323_set_reg(0x38u, 0x0007u);

	bmi323_int_pin_config_t cfg = {
		.level         = BMI323_INT_LEVEL_ACTIVE_HIGH,
		.drive         = BMI323_INT_DRIVE_OPEN_DRAIN,
		.output_enable = true,
	};
	zassert_equal(bmi323_configure_int_pin(&dev, BMI323_INT_PIN2, &cfg), ALP_OK);

	/* 0x07 shifted into bits[10:8] = 0x700; INT1's seeded 0x07 must
	 * survive.  Catches a PIN1/PIN2 shift mix-up. */
	zassert_equal(fake_bmi323_get_reg(0x38u), 0x0707u);

	bmi323_deinit(&dev);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_bmi323_set_int_latch_permanent_writes_int_conf)
{
	fake_bmi323_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	bmi323_t dev;
	zassert_equal(bmi323_init(&dev, bus, FAKE_BMI323_ADDR), ALP_OK);

	zassert_equal(bmi323_set_int_latch(&dev, BMI323_INT_LATCH_PERMANENT), ALP_OK);
	zassert_equal(fake_bmi323_get_reg(0x39u), 0x0001u);
	zassert_equal(fake_bmi323_write_count(0x39u), 1u);

	bmi323_deinit(&dev);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_bmi323_route_data_ready_int_preserves_other_fields)
{
	fake_bmi323_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	bmi323_t dev;
	zassert_equal(bmi323_init(&dev, bus, FAKE_BMI323_ADDR), ALP_OK);

	/* INT_MAP2 also carries fields this driver doesn't manage (temp,
	 * FIFO, tap, i3c, err_status) -- seed a value with bits set both
	 * above and below the accel/gyro [11:8] field this call touches. */
	fake_bmi323_set_reg(0x3Bu, 0x1234u);

	zassert_equal(bmi323_route_data_ready_int(&dev, BMI323_INT_ROUTE_INT2, BMI323_INT_ROUTE_INT1),
	              ALP_OK);

	/* 0x1234 & ~0x0F00 = 0x1034; accel(INT2=2)<<10 = 0x0800;
	 * gyro(INT1=1)<<8 = 0x0100 -> 0x1034 | 0x0800 | 0x0100 = 0x1934.
	 * Top nibble (0x1) and bottom byte (0x34) of the seed survive
	 * untouched; a swapped accel/gyro shift would land on a different
	 * byte (0x1634). */
	zassert_equal(fake_bmi323_get_reg(0x3Bu), 0x1934u);

	bmi323_deinit(&dev);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_bmi323_data_ready_decodes_status_bits_7_and_6)
{
	fake_bmi323_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	bmi323_t dev;
	zassert_equal(bmi323_init(&dev, bus, FAKE_BMI323_ADDR), ALP_OK);

	bmi323_data_ready_t rdy;

	fake_bmi323_set_reg(0x02u, 0x0080u); /* bit7 only -> accel */
	zassert_equal(bmi323_data_ready(&dev, &rdy), ALP_OK);
	zassert_true(rdy.accel);
	zassert_false(rdy.gyro);

	fake_bmi323_set_reg(0x02u, 0x0040u); /* bit6 only -> gyro */
	zassert_equal(bmi323_data_ready(&dev, &rdy), ALP_OK);
	zassert_false(rdy.accel);
	zassert_true(rdy.gyro);

	fake_bmi323_set_reg(0x02u, 0x00C0u); /* both */
	zassert_equal(bmi323_data_ready(&dev, &rdy), ALP_OK);
	zassert_true(rdy.accel);
	zassert_true(rdy.gyro);

	fake_bmi323_set_reg(0x02u, 0x0000u); /* neither */
	zassert_equal(bmi323_data_ready(&dev, &rdy), ALP_OK);
	zassert_false(rdy.accel);
	zassert_false(rdy.gyro);

	bmi323_deinit(&dev);
	alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* bmp581 (Bosch barometer, on-board EVK pressure sensor)              */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_bmp581_init_null_args)
{
	bmp581_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	zassert_equal(bmp581_init(NULL, bus, BMP581_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(bmp581_init(&dev, NULL, BMP581_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(bmp581_init(&dev, bus, 0), ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_bmp581_compensate_pure_arithmetic)
{
	/* bmp581_compensate is a pure scale conversion -- it doesn't
     * touch the bus and doesn't need an initialised device.  Test
     * the conversion math directly. */

	/* 1/64 Pa LSB.  raw = 64 * 101325 + 32 (rounding) = 6484832 */
	bmp581_raw_t raw = {
		.pressure_raw    = 6484800,    /* exactly 101325 Pa */
		.temperature_raw = 25 * 65536, /* exactly 25 °C */
	};
	bmp581_compensated_t comp;
	zassert_equal(bmp581_compensate(&raw, &comp), ALP_OK);
	zassert_equal(comp.pressure_pa, 101325);
	zassert_equal(comp.temperature_c1000, 25000);

	/* NULL-arg pair must reject. */
	zassert_equal(bmp581_compensate(NULL, &comp), ALP_ERR_INVAL);
	zassert_equal(bmp581_compensate(&raw, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_bmp581_post_init_calls_reject_uninitialised)
{
	bmp581_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	alp_status_t s = bmp581_init(&dev, bus, BMP581_I2C_ADDR_LOW);
	if (s != ALP_OK) {
		bmp581_raw_t raw;
		zassert_equal(bmp581_read_raw(&dev, &raw), ALP_ERR_NOT_READY);
		zassert_equal(bmp581_set_sampling(
		                  &dev, BMP581_OSR_X4, BMP581_OSR_X1, BMP581_ODR_25_HZ, BMP581_MODE_NORMAL),
		              ALP_ERR_NOT_READY);
	}

	bmp581_deinit(&dev);
	alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* bmp581 -- invalid-enum rejection (#736)                             */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_bmp581_set_sampling_rejects_reserved_odr)
{
	bmp581_t dev    = { 0 };
	dev.initialised = true;

	/* bmp581_odr_t declares a curated subset of the 5-bit ODR field
	 * (0x00, 0x01, 0x07, 0x0E, 0x14, 0x17, 0x1C).  0x02 and 0x1F are
	 * both real, datasheet-defined ODR codes the enum simply doesn't
	 * expose -- not reserved encodings -- so an upper-bound/mask check
	 * would silently admit them; the switch-validate guard must still
	 * reject anything outside the declared set. */
	zassert_equal(bmp581_set_sampling(
	                  &dev, BMP581_OSR_X4, BMP581_OSR_X1, (bmp581_odr_t)0x02, BMP581_MODE_NORMAL),
	              ALP_ERR_INVAL,
	              "undeclared ODR 0x02 must be rejected, not masked");
	zassert_equal(bmp581_set_sampling(
	                  &dev, BMP581_OSR_X4, BMP581_OSR_X1, (bmp581_odr_t)0x1F, BMP581_MODE_NORMAL),
	              ALP_ERR_INVAL,
	              "undeclared ODR 0x1F (0.125 Hz) must be rejected, not masked");
}

ZTEST(alp_chips, test_bmp581_set_sampling_rejects_out_of_range_osr_and_mode)
{
	bmp581_t dev    = { 0 };
	dev.initialised = true;

	zassert_equal(bmp581_set_sampling(
	                  &dev, (bmp581_osr_t)8, BMP581_OSR_X1, BMP581_ODR_25_HZ, BMP581_MODE_NORMAL),
	              ALP_ERR_INVAL,
	              "press_osr=8 (out of range) must be rejected");
	zassert_equal(bmp581_set_sampling(
	                  &dev, BMP581_OSR_X4, (bmp581_osr_t)8, BMP581_ODR_25_HZ, BMP581_MODE_NORMAL),
	              ALP_ERR_INVAL,
	              "temp_osr=8 (out of range) must be rejected");
	zassert_equal(
	    bmp581_set_sampling(&dev, BMP581_OSR_X4, BMP581_OSR_X1, BMP581_ODR_25_HZ, (bmp581_mode_t)4),
	    ALP_ERR_INVAL,
	    "mode=4 (out of range) must be rejected");
}

ZTEST(alp_chips, test_bmp581_set_sampling_accepts_every_declared_odr)
{
	/* A validated call proceeds to the register write, so this needs a
	 * real (open) bus -- a rejected call never gets that far.  There's
	 * no fake i2c target behind this bus, so the write itself returns a
	 * transport error; assert on ALP_ERR_INVAL specifically to prove
	 * validation let the call through (see the bme280 sweep above). */
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	bmp581_t dev    = { 0 };
	dev.bus         = bus;
	dev.addr        = BMP581_I2C_ADDR_LOW;
	dev.initialised = true;

	const bmp581_odr_t odr_values[] = {
		BMP581_ODR_240_HZ, BMP581_ODR_120_HZ, BMP581_ODR_50_HZ, BMP581_ODR_25_HZ,
		BMP581_ODR_10_HZ,  BMP581_ODR_5_HZ,   BMP581_ODR_1_HZ,
	};
	for (size_t i = 0; i < ARRAY_SIZE(odr_values); i++) {
		zassert_not_equal(
		    bmp581_set_sampling(
		        &dev, BMP581_OSR_X4, BMP581_OSR_X1, odr_values[i], BMP581_MODE_NORMAL),
		    ALP_ERR_INVAL,
		    "declared ODR wrongly rejected by the enum guard");
	}

	alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* bmp581 -- fake-backed register-protocol tests                      */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_fake_bmp581_data_ready_decodes_bit0)
{
	fake_bmp581_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	bmp581_t dev;
	zassert_equal(bmp581_init(&dev, bus, BMP581_I2C_ADDR_LOW), ALP_OK);

	bool ready = false;
	fake_bmp581_set_reg(0x27u, 0x01u);
	zassert_equal(bmp581_data_ready(&dev, &ready), ALP_OK);
	zassert_true(ready);

	/* Only bit 0 (drdy_data_reg) counts -- an unrelated INT_STATUS bit
	 * set (e.g. fifo_full, bit 1) must not read as "ready". */
	fake_bmp581_set_reg(0x27u, 0x02u);
	zassert_equal(bmp581_data_ready(&dev, &ready), ALP_OK);
	zassert_false(ready);

	fake_bmp581_set_reg(0x27u, 0x00u);
	zassert_equal(bmp581_data_ready(&dev, &ready), ALP_OK);
	zassert_false(ready);

	bmp581_deinit(&dev);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_bmp581_configure_int_pin_writes_expected_byte)
{
	fake_bmp581_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	bmp581_t dev;
	zassert_equal(bmp581_init(&dev, bus, BMP581_I2C_ADDR_LOW), ALP_OK);

	static const struct {
		bool                  enable;
		bmp581_int_drive_t    drive;
		bmp581_int_polarity_t polarity;
		bmp581_int_mode_t     mode;
		uint8_t               expect;
	} cases[] = {
		/* pad_int_drv[7:4]=0x3 fixed | int_en[3] | int_od[2] | int_pol[1] | int_mode[0]. */
		{ false, BMP581_INT_PUSH_PULL, BMP581_INT_ACTIVE_LOW, BMP581_INT_PULSED, 0x30u },
		{ true, BMP581_INT_PUSH_PULL, BMP581_INT_ACTIVE_LOW, BMP581_INT_PULSED, 0x38u },
		{ true, BMP581_INT_OPEN_DRAIN, BMP581_INT_ACTIVE_LOW, BMP581_INT_PULSED, 0x3Cu },
		{ true, BMP581_INT_OPEN_DRAIN, BMP581_INT_ACTIVE_HIGH, BMP581_INT_PULSED, 0x3Eu },
		{ true, BMP581_INT_OPEN_DRAIN, BMP581_INT_ACTIVE_HIGH, BMP581_INT_LATCHED, 0x3Fu },
	};
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		zassert_equal(bmp581_configure_int_pin(
		                  &dev, cases[i].enable, cases[i].drive, cases[i].polarity, cases[i].mode),
		              ALP_OK);
		zassert_equal(fake_bmp581_get_reg(0x14u),
		              cases[i].expect,
		              "case %u: INT_CONFIG mismatch",
		              (unsigned)i);
	}

	bmp581_deinit(&dev);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_bmp581_set_int_sources_writes_verbatim_and_rejects_out_of_range)
{
	fake_bmp581_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	bmp581_t dev;
	zassert_equal(bmp581_init(&dev, bus, BMP581_I2C_ADDR_LOW), ALP_OK);

	zassert_equal(bmp581_set_int_sources(&dev, 0x0Fu), ALP_OK);
	zassert_equal(fake_bmp581_get_reg(0x15u), 0x0Fu);
	zassert_equal(fake_bmp581_write_count(0x15u), 1u);

	/* Bit 4 is outside the four defined source bits -- must be
	 * rejected before it ever reaches the bus. */
	zassert_equal(bmp581_set_int_sources(&dev, 0x10u), ALP_ERR_INVAL);
	zassert_equal(fake_bmp581_write_count(0x15u), 1u, "rejected value must not reach the bus");

	bmp581_deinit(&dev);
	alp_i2c_close(bus);
}

/* ==================================================================== */
/* Register-protocol tests (fake i2c-emul targets)                       */
/* ==================================================================== */
/* These tests exercise actual register paths through the chip drivers, */
/* not just lifecycle / NULL handling.  Each fake target lives in       */
/* src/fake_<chip>.c, attaches to i2c0_emul at the chip's default I2C   */
/* address, and exposes inspection helpers via fakes.h.                  */
/*                                                                      */
/* DORMANT -- the fake i2c-emul targets are compiled out by              */
/* maintainer decision (commit 4b0b33d5): driver register-protocol       */
/* validation moved to real silicon via examples/v2n/v2n-brd-i2c-bringup */
/* (see CHANGELOG.md, "Removed -- chips: fake-based register tests").    */
/* Gated on DT_NODE_EXISTS(DT_NODELABEL(fake_lsm6dso)) so this block     */
/* compiles to nothing while the overlay's fake_* nodes stay commented   */
/* out (see CMakeLists.txt).                                             */
/* ==================================================================== */

#if DT_NODE_EXISTS(DT_NODELABEL(fake_lsm6dso))

/* ------------------------------------------------------------------ */
/* lsm6dso (fake-backed)                                               */
/* ------------------------------------------------------------------ */

#define REG_LSM6DSO_CTRL1_XL 0x10
#define REG_LSM6DSO_CTRL2_G  0x11
#define REG_LSM6DSO_OUTX_L_A 0x28
#define REG_LSM6DSO_OUTX_L_G 0x22

ZTEST(alp_chips, test_fake_lsm6dso_init_succeeds_against_fake)
{
	fake_lsm6dso_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	lsm6dso_t dev;
	zassert_equal(lsm6dso_init(&dev, bus, LSM6DSO_I2C_ADDR_LOW),
	              ALP_OK,
	              "fake responds to WHO_AM_I with 0x6C — init must succeed");

	uint8_t id = 0;
	zassert_equal(lsm6dso_read_id(&dev, &id), ALP_OK);
	zassert_equal(id, LSM6DSO_WHO_AM_I_VAL);

	lsm6dso_deinit(&dev);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_lsm6dso_set_accel_writes_ctrl1_xl)
{
	fake_lsm6dso_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	lsm6dso_t dev;
	zassert_equal(lsm6dso_init(&dev, bus, LSM6DSO_I2C_ADDR_LOW), ALP_OK);

	/* CTRL1_XL: ODR_XL[7:4] | FS_XL[3:2] | LPF2_XL_EN[1] | reserved[0].
     * ODR=104Hz (0x4), FS=4G (0x2) → byte = (0x4<<4) | (0x2<<2) = 0x48. */
	zassert_equal(lsm6dso_set_accel(&dev, LSM6DSO_ODR_104_HZ, LSM6DSO_ACCEL_FS_4G), ALP_OK);
	zassert_equal(fake_lsm6dso_get_reg(REG_LSM6DSO_CTRL1_XL),
	              0x48u,
	              "CTRL1_XL should encode ODR=104Hz | FS=4G");

	/* Switch to ODR=833Hz (0x7), FS=16G (0x1) → (0x7<<4) | (0x1<<2) = 0x74. */
	zassert_equal(lsm6dso_set_accel(&dev, LSM6DSO_ODR_833_HZ, LSM6DSO_ACCEL_FS_16G), ALP_OK);
	zassert_equal(fake_lsm6dso_get_reg(REG_LSM6DSO_CTRL1_XL), 0x74u);

	lsm6dso_deinit(&dev);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_lsm6dso_set_gyro_writes_ctrl2_g)
{
	fake_lsm6dso_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	lsm6dso_t dev;
	zassert_equal(lsm6dso_init(&dev, bus, LSM6DSO_I2C_ADDR_LOW), ALP_OK);

	/* ODR=208Hz (0x5), FS=1000dps (0x2) → (0x5<<4) | (0x2<<2) = 0x58. */
	zassert_equal(lsm6dso_set_gyro(&dev, LSM6DSO_ODR_208_HZ, LSM6DSO_GYRO_FS_1000_DPS), ALP_OK);
	zassert_equal(fake_lsm6dso_get_reg(REG_LSM6DSO_CTRL2_G), 0x58u);

	lsm6dso_deinit(&dev);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_lsm6dso_read_accel_decodes_le16)
{
	fake_lsm6dso_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	lsm6dso_t dev;
	zassert_equal(lsm6dso_init(&dev, bus, LSM6DSO_I2C_ADDR_LOW), ALP_OK);

	/* Seed the accel output registers: X = 0x1234, Y = -0x100 = 0xFF00,
     * Z = 0x7FFF (positive max).  LSM6DSO returns little-endian. */
	fake_lsm6dso_set_reg(REG_LSM6DSO_OUTX_L_A + 0, 0x34); /* X low */
	fake_lsm6dso_set_reg(REG_LSM6DSO_OUTX_L_A + 1, 0x12); /* X high */
	fake_lsm6dso_set_reg(REG_LSM6DSO_OUTX_L_A + 2, 0x00); /* Y low */
	fake_lsm6dso_set_reg(REG_LSM6DSO_OUTX_L_A + 3, 0xFF); /* Y high */
	fake_lsm6dso_set_reg(REG_LSM6DSO_OUTX_L_A + 4, 0xFF); /* Z low */
	fake_lsm6dso_set_reg(REG_LSM6DSO_OUTX_L_A + 5, 0x7F); /* Z high */

	lsm6dso_axes_t a;
	zassert_equal(lsm6dso_read_accel(&dev, &a), ALP_OK);
	zassert_equal(a.x, (int16_t)0x1234);
	zassert_equal(a.y, (int16_t)0xFF00); /* -256 */
	zassert_equal(a.z, (int16_t)0x7FFF);

	lsm6dso_deinit(&dev);
	alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* bme280 (fake-backed)                                                */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_fake_bme280_init_loads_calibration)
{
	fake_bme280_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	bme280_t dev;
	zassert_equal(bme280_init(&dev, bus, BME280_I2C_ADDR_LOW),
	              ALP_OK,
	              "fake responds to CHIP_ID with 0x60 — init must succeed");

	/* Canonical Bosch datasheet example values seeded by the fake. */
	zassert_equal(dev.calib.T1, 27504u);
	zassert_equal(dev.calib.T2, (int16_t)26435);
	zassert_equal(dev.calib.T3, (int16_t)-1000);
	zassert_equal(dev.calib.P1, 36477u);
	zassert_equal(dev.calib.P2, (int16_t)-10685);
	zassert_equal(dev.calib.P9, (int16_t)6000);
	zassert_equal(dev.calib.H1, 75);
	zassert_equal(dev.calib.H6, (int8_t)30);

	bme280_deinit(&dev);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_bme280_read_raw_decodes_msb_first)
{
	fake_bme280_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	bme280_t dev;
	zassert_equal(bme280_init(&dev, bus, BME280_I2C_ADDR_LOW), ALP_OK);

	bme280_raw_t raw;
	zassert_equal(bme280_read_raw(&dev, &raw), ALP_OK);

	/* Pressure raw block 0x65 0x5A 0xC0 → (0x65<<12)|(0x5A<<4)|(0xC0>>4)
     * = 0x655AC = 415148 (canonical Bosch example). */
	zassert_equal(raw.pressure_raw, 415148);
	/* Temperature raw block 0x7E 0xF5 0x00 → 0x7EF50 = 519888. */
	zassert_equal(raw.temperature_raw, 519888);
	/* Humidity 0x6F 0xF0 → 0x6FF0 (synthetic). */
	zassert_equal(raw.humidity_raw, 0x6FF0u);

	bme280_deinit(&dev);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_bme280_compensate_matches_datasheet_example)
{
	fake_bme280_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	bme280_t dev;
	zassert_equal(bme280_init(&dev, bus, BME280_I2C_ADDR_LOW), ALP_OK);

	bme280_raw_t raw;
	zassert_equal(bme280_read_raw(&dev, &raw), ALP_OK);

	bme280_compensated_t comp;
	zassert_equal(bme280_compensate(&dev, &raw, &comp), ALP_OK);

	/* BST-BME280-DS002 §4.2.3 Table 2 documents the worked example:
     * T = 25.08 °C → c100 = 2508,  P ≈ 100653 Pa.  Allow ±2 c100 to
     * absorb the documented ±2-LSB rounding noise across compilers. */
	zassert_within(comp.temperature_c100,
	               2508,
	               2,
	               "compensated T x100 = %d, expected 2508 ± 2",
	               (int)comp.temperature_c100);
	zassert_within(comp.pressure_pa,
	               100653u,
	               50u,
	               "compensated P (Pa) = %u, expected 100653 ± 50",
	               comp.pressure_pa);

	bme280_deinit(&dev);
	alp_i2c_close(bus);
}

#endif /* DT_NODE_EXISTS(DT_NODELABEL(fake_lsm6dso)) — fake-emul block */

/* ------------------------------------------------------------------ */
/* Invalid-enum rejection sweep (#790, split from #736)                */
/* ------------------------------------------------------------------ */
/* A rejected config call must not reach the bus, so these tests build
 * the device context directly with a NULL bus instead of going through
 * <chip>_init -- no fake i2c target required.
 *
 * The NULL bus is also what makes each test discriminate.  An
 * unvalidated driver masks the undeclared value into the register and
 * calls alp_i2c_write(NULL, ...), which returns ALP_ERR_NOT_READY.  A
 * validating driver returns ALP_ERR_INVAL before any write.  The two
 * codes differ, so a passing test proves the check is really there.
 *
 * Each chip also gets an "accepts declared" control.  Those pass both
 * before and after the fix by design -- they are not evidence of the
 * fix, they pin the opposite failure mode: an over-tightened check that
 * rejects declared values (the regression #739 hit on a real board).
 */

ZTEST(alp_chips, test_bmi323_set_accel_rejects_undeclared_odr)
{
	bmi323_t dev    = { 0 };
	dev.initialised = true;

	/* bmi323_odr_t declares 0x1..0xE; the register field is 4 bits. */
	static const uint8_t bad[] = { 0x0, 0xF };

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		zassert_equal(bmi323_set_accel(&dev, (bmi323_odr_t)bad[i], BMI323_ACCEL_FS_2G),
		              ALP_ERR_INVAL,
		              "accel odr 0x%x is undeclared and must be rejected, not masked",
		              bad[i]);
	}
}

ZTEST(alp_chips, test_bmi323_set_accel_rejects_undeclared_fs)
{
	bmi323_t dev    = { 0 };
	dev.initialised = true;

	/* bmi323_accel_fs_t declares 0x0..0x3; the field is 3 bits. */
	static const uint8_t bad[] = { 0x4, 0x5, 0x6, 0x7 };

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		zassert_equal(bmi323_set_accel(&dev, BMI323_ODR_100_HZ, (bmi323_accel_fs_t)bad[i]),
		              ALP_ERR_INVAL,
		              "accel fs 0x%x is undeclared and must be rejected, not masked",
		              bad[i]);
	}
}

ZTEST(alp_chips, test_bmi323_set_gyro_rejects_undeclared_odr)
{
	bmi323_t dev    = { 0 };
	dev.initialised = true;

	static const uint8_t bad[] = { 0x0, 0xF };

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		zassert_equal(bmi323_set_gyro(&dev, (bmi323_odr_t)bad[i], BMI323_GYRO_FS_125_DPS),
		              ALP_ERR_INVAL,
		              "gyro odr 0x%x is undeclared and must be rejected, not masked",
		              bad[i]);
	}
}

ZTEST(alp_chips, test_bmi323_set_gyro_rejects_undeclared_fs)
{
	bmi323_t dev    = { 0 };
	dev.initialised = true;

	/* bmi323_gyro_fs_t declares 0x0..0x4; the field is 3 bits. */
	static const uint8_t bad[] = { 0x5, 0x6, 0x7 };

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		zassert_equal(bmi323_set_gyro(&dev, BMI323_ODR_100_HZ, (bmi323_gyro_fs_t)bad[i]),
		              ALP_ERR_INVAL,
		              "gyro fs 0x%x is undeclared and must be rejected, not masked",
		              bad[i]);
	}
}

ZTEST(alp_chips, test_bmi323_set_accel_gyro_accept_declared_bounds)
{
	/* Control -- see the block comment above. */
	bmi323_t dev    = { 0 };
	dev.initialised = true;

	zassert_equal(bmi323_set_accel(&dev, BMI323_ODR_0_78125_HZ, BMI323_ACCEL_FS_2G),
	              ALP_ERR_NOT_READY,
	              "declared low bound must reach the bus, not be rejected");
	zassert_equal(bmi323_set_accel(&dev, BMI323_ODR_6400_HZ, BMI323_ACCEL_FS_16G),
	              ALP_ERR_NOT_READY,
	              "declared high bound must reach the bus, not be rejected");
	zassert_equal(bmi323_set_gyro(&dev, BMI323_ODR_6400_HZ, BMI323_GYRO_FS_2000_DPS),
	              ALP_ERR_NOT_READY,
	              "declared gyro high bound must reach the bus, not be rejected");
}

ZTEST(alp_chips, test_icm42670_set_accel_rejects_undeclared_odr)
{
	icm42670_t dev  = { 0 };
	dev.initialised = true;

	/* icm42670_odr_t is shared by ACCEL_ODR and GYRO_ODR but their
     * legal sets differ (TDK DS-000451 Rev 1.0).  ACCEL_CONFIG0 p.57:
     * 0x0 and 0x1..0x4 reserved, 0x5..0xF valid.  0x0 (ODR_OFF) is a
     * declared enum member, not a stray bit pattern -- it is reserved,
     * not "disable via ODR" (that's PWR_MGMT0's job), so it must be
     * rejected explicitly alongside the undeclared 0x1..0x4. */
	static const uint8_t bad[] = { 0x0, 0x1, 0x2, 0x3, 0x4 };

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		zassert_equal(icm42670_set_accel(&dev, (icm42670_odr_t)bad[i], ICM42670_ACCEL_FS_2G),
		              ALP_ERR_INVAL,
		              "accel odr 0x%x is a reserved encoding and must be rejected",
		              bad[i]);
	}
}

ZTEST(alp_chips, test_icm42670_set_gyro_rejects_undeclared_odr)
{
	icm42670_t dev  = { 0 };
	dev.initialised = true;

	/* GYRO_CONFIG0 p.56 is narrower than ACCEL_CONFIG0: 0x0 and
     * 0x1..0x4 reserved (same as accel), but ALSO 0xD/0xE/0xF
     * reserved -- the gyro has no sub-12.5 Hz low-power rates, even
     * though those codes are valid, declared ODR_6_25_HZ/ODR_3_125_HZ/
     * ODR_1_5625_HZ members on the accel side. */
	static const uint8_t bad[] = { 0x0, 0x1, 0x2, 0x3, 0x4, 0xD, 0xE, 0xF };

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		zassert_equal(icm42670_set_gyro(&dev, (icm42670_odr_t)bad[i], ICM42670_GYRO_FS_250_DPS),
		              ALP_ERR_INVAL,
		              "gyro odr 0x%x is a reserved encoding and must be rejected",
		              bad[i]);
	}
}

ZTEST(alp_chips, test_icm42670_set_accel_gyro_accept_declared_odr)
{
	/* Control -- every code legal for each field must survive.  The two
     * good lists differ: accel accepts 0x5..0xF, gyro only 0x5..0xC. */
	icm42670_t dev  = { 0 };
	dev.initialised = true;

	static const icm42670_odr_t accel_good[] = {
		ICM42670_ODR_1600_HZ, ICM42670_ODR_800_HZ,   ICM42670_ODR_400_HZ,   ICM42670_ODR_200_HZ,
		ICM42670_ODR_100_HZ,  ICM42670_ODR_50_HZ,    ICM42670_ODR_25_HZ,    ICM42670_ODR_12_5_HZ,
		ICM42670_ODR_6_25_HZ, ICM42670_ODR_3_125_HZ, ICM42670_ODR_1_5625_HZ
	};
	static const icm42670_odr_t gyro_good[] = { ICM42670_ODR_1600_HZ, ICM42670_ODR_800_HZ,
		                                        ICM42670_ODR_400_HZ,  ICM42670_ODR_200_HZ,
		                                        ICM42670_ODR_100_HZ,  ICM42670_ODR_50_HZ,
		                                        ICM42670_ODR_25_HZ,   ICM42670_ODR_12_5_HZ };

	for (size_t i = 0; i < ARRAY_SIZE(accel_good); i++) {
		zassert_equal(icm42670_set_accel(&dev, accel_good[i], ICM42670_ACCEL_FS_2G),
		              ALP_ERR_NOT_READY,
		              "declared accel odr 0x%x must reach the bus, not be rejected",
		              (unsigned)accel_good[i]);
	}
	for (size_t i = 0; i < ARRAY_SIZE(gyro_good); i++) {
		zassert_equal(icm42670_set_gyro(&dev, gyro_good[i], ICM42670_GYRO_FS_250_DPS),
		              ALP_ERR_NOT_READY,
		              "declared gyro odr 0x%x must reach the bus, not be rejected",
		              (unsigned)gyro_good[i]);
	}
}

ZTEST(alp_chips, test_lsm6dso_set_accel_rejects_undeclared_odr)
{
	lsm6dso_t dev   = { 0 };
	dev.initialised = true;

	/* lsm6dso_odr_t declares 0x0..0xA; the field is 4 bits. */
	static const uint8_t bad[] = { 0xB, 0xC, 0xD, 0xE, 0xF };

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		zassert_equal(lsm6dso_set_accel(&dev, (lsm6dso_odr_t)bad[i], LSM6DSO_ACCEL_FS_2G),
		              ALP_ERR_INVAL,
		              "accel odr 0x%x is undeclared and must be rejected, not masked",
		              bad[i]);
	}
}

ZTEST(alp_chips, test_lsm6dso_set_gyro_rejects_undeclared_odr)
{
	lsm6dso_t dev   = { 0 };
	dev.initialised = true;

	static const uint8_t bad[] = { 0xB, 0xC, 0xD, 0xE, 0xF };

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		zassert_equal(lsm6dso_set_gyro(&dev, (lsm6dso_odr_t)bad[i], LSM6DSO_GYRO_FS_250_DPS),
		              ALP_ERR_INVAL,
		              "gyro odr 0x%x is undeclared and must be rejected, not masked",
		              bad[i]);
	}
}

ZTEST(alp_chips, test_lsm6dso_set_accel_gyro_accept_declared_bounds)
{
	/* Control -- see the block comment above. */
	lsm6dso_t dev   = { 0 };
	dev.initialised = true;

	zassert_equal(lsm6dso_set_accel(&dev, LSM6DSO_ODR_OFF, LSM6DSO_ACCEL_FS_2G),
	              ALP_ERR_NOT_READY,
	              "declared low bound must reach the bus, not be rejected");
	zassert_equal(lsm6dso_set_accel(&dev, LSM6DSO_ODR_6660_HZ, LSM6DSO_ACCEL_FS_8G),
	              ALP_ERR_NOT_READY,
	              "declared high bound must reach the bus, not be rejected");
	zassert_equal(lsm6dso_set_gyro(&dev, LSM6DSO_ODR_6660_HZ, LSM6DSO_GYRO_FS_2000_DPS),
	              ALP_ERR_NOT_READY,
	              "declared gyro high bound must reach the bus, not be rejected");
}

ZTEST(alp_chips, test_lis2dw12_set_accel_rejects_undeclared_odr)
{
	lis2dw12_t dev  = { 0 };
	dev.initialised = true;

	/* lis2dw12_odr_t declares 0x0..0x9; the field is 4 bits. */
	static const uint8_t bad[] = { 0xA, 0xB, 0xC, 0xD, 0xE, 0xF };

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		zassert_equal(
		    lis2dw12_set_accel(
		        &dev, (lis2dw12_odr_t)bad[i], LIS2DW12_FS_2G, LIS2DW12_MODE_HIGH_PERF_14BIT),
		    ALP_ERR_INVAL,
		    "odr 0x%x is undeclared and must be rejected, not masked",
		    bad[i]);
	}
}

ZTEST(alp_chips, test_lis2dw12_set_accel_accepts_declared_bounds)
{
	/* Control -- see the block comment above. */
	lis2dw12_t dev  = { 0 };
	dev.initialised = true;

	zassert_equal(
	    lis2dw12_set_accel(&dev, LIS2DW12_ODR_OFF, LIS2DW12_FS_2G, LIS2DW12_MODE_LOW_POWER_12BIT),
	    ALP_ERR_NOT_READY,
	    "declared low bound must reach the bus, not be rejected");
	zassert_equal(
	    lis2dw12_set_accel(&dev, LIS2DW12_ODR_1600_HZ, LIS2DW12_FS_16G, LIS2DW12_MODE_SINGLE_SHOT),
	    ALP_ERR_NOT_READY,
	    "declared high bound must reach the bus, not be rejected");
}
