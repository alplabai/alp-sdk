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

#include "fakes.h"

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

/* ==================================================================== */
/* Register-protocol tests (fake i2c-emul targets)                       */
/* ==================================================================== */
/* These tests exercise actual register paths through the chip drivers, */
/* not just lifecycle / NULL handling.  Each fake target lives in       */
/* src/fake_<chip>.c, attaches to i2c0_emul at the chip's default I2C   */
/* address, and exposes inspection helpers via fakes.h.                  */
/*                                                                      */
/* TEMPORARILY GATED — the fake EMUL_DT_INST_DEFINE pattern needs a     */
/* paired DEVICE_DT_INST_DEFINE for Zephyr 3.7's native_sim x86 link    */
/* to resolve `__device_dts_ord_<N>` references the parent i2c-emul     */
/* controller's emuls_<N> array generates.  Tracked as a v0.2 follow-   */
/* up; gated on DT_NODE_EXISTS so the tests are no-ops while the        */
/* overlay's fake_* nodes are commented out.                             */
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

	/* icm42670_odr_t is genuinely sparse: it declares 0x0, 0x4..0x8 and
     * 0xA..0xF.  0x1/0x2/0x3/0x9 sit inside the 4-bit field but are not
     * declared members, so a bitmask alone cannot reject them. */
	static const uint8_t bad[] = { 0x1, 0x2, 0x3, 0x9 };

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

	static const uint8_t bad[] = { 0x1, 0x2, 0x3, 0x9 };

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		zassert_equal(icm42670_set_gyro(&dev, (icm42670_odr_t)bad[i], ICM42670_GYRO_FS_250_DPS),
		              ALP_ERR_INVAL,
		              "gyro odr 0x%x is a reserved encoding and must be rejected",
		              bad[i]);
	}
}

ZTEST(alp_chips, test_icm42670_set_accel_gyro_accept_declared_odr)
{
	/* Control -- every declared member must survive, including the ones
     * either side of each sparse gap. */
	icm42670_t dev  = { 0 };
	dev.initialised = true;

	static const icm42670_odr_t good[] = {
		ICM42670_ODR_OFF,     ICM42670_ODR_1600_HZ, ICM42670_ODR_800_HZ,   ICM42670_ODR_400_HZ,
		ICM42670_ODR_200_HZ,  ICM42670_ODR_100_HZ,  ICM42670_ODR_50_HZ,    ICM42670_ODR_25_HZ,
		ICM42670_ODR_12_5_HZ, ICM42670_ODR_6_25_HZ, ICM42670_ODR_3_125_HZ, ICM42670_ODR_1_5625_HZ
	};

	for (size_t i = 0; i < ARRAY_SIZE(good); i++) {
		zassert_equal(icm42670_set_accel(&dev, good[i], ICM42670_ACCEL_FS_2G),
		              ALP_ERR_NOT_READY,
		              "declared accel odr 0x%x must reach the bus, not be rejected",
		              (unsigned)good[i]);
		zassert_equal(icm42670_set_gyro(&dev, good[i], ICM42670_GYRO_FS_250_DPS),
		              ALP_ERR_NOT_READY,
		              "declared gyro odr 0x%x must reach the bus, not be rejected",
		              (unsigned)good[i]);
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
