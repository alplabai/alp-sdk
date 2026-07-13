/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v0.5 §D.industrial batch -- NULL-arg guard smokes for industrial
 * sensing/control chips: barometers, ToF/ultrasonic ranging, motor
 * drivers, magnetic encoders, load-cell/thermocouple ADCs, and
 * light/magnetometer sensors.
 */

#include <zephyr/ztest.h>

#include "alp/chips/a02yyuw.h"
#include "alp/chips/a4988.h"
#include "alp/chips/as5048a_b.h"
#include "alp/chips/bmp390.h"
#include "alp/chips/drv8825.h"
#include "alp/chips/drv8833.h"
#include "alp/chips/hx711.h"
#include "alp/chips/lps22hb.h"
#include "alp/chips/max31855.h"
#include "alp/chips/max31865.h"
#include "alp/chips/ms5611.h"
#include "alp/chips/mt6701.h"
#include "alp/chips/qmc5883l.h"
#include "alp/chips/tmc2209.h"
#include "alp/chips/tsl2591.h"
#include "alp/chips/veml7700.h"
#include "alp/chips/vl53l1x.h"
#include "alp/chips/vl53l5cx.h"
#include "alp/e1m_pinout.h"
#include "alp/peripheral.h"

ZTEST(alp_chips, test_bmp390_init_null_args)
{
	bmp390_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(bmp390_init(NULL, bus, BMP390_I2C_ADDR_PRIMARY), ALP_ERR_INVAL);
	zassert_equal(bmp390_init(&dev, NULL, BMP390_I2C_ADDR_PRIMARY), ALP_ERR_INVAL);
	zassert_equal(bmp390_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ms5611_init_null_args)
{
	ms5611_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(ms5611_init(NULL, bus, MS5611_I2C_ADDR_PRIMARY), ALP_ERR_INVAL);
	zassert_equal(ms5611_init(&dev, NULL, MS5611_I2C_ADDR_PRIMARY), ALP_ERR_INVAL);
	zassert_equal(ms5611_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_lps22hb_init_null_args)
{
	lps22hb_t  dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(lps22hb_init(NULL, bus, LPS22HB_I2C_ADDR_HIGH), ALP_ERR_INVAL);
	zassert_equal(lps22hb_init(&dev, NULL, LPS22HB_I2C_ADDR_HIGH), ALP_ERR_INVAL);
	zassert_equal(lps22hb_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_vl53l1x_init_null_args)
{
	vl53l1x_t  dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(vl53l1x_init(NULL, bus, VL53L1X_I2C_ADDR_DEFAULT), ALP_ERR_INVAL);
	zassert_equal(vl53l1x_init(&dev, NULL, VL53L1X_I2C_ADDR_DEFAULT), ALP_ERR_INVAL);
	zassert_equal(vl53l1x_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_vl53l5cx_init_null_args)
{
	vl53l5cx_t dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(vl53l5cx_init(NULL, bus, VL53L5CX_I2C_ADDR_DEFAULT), ALP_ERR_INVAL);
	zassert_equal(vl53l5cx_init(&dev, NULL, VL53L5CX_I2C_ADDR_DEFAULT), ALP_ERR_INVAL);
	zassert_equal(vl53l5cx_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_a02yyuw_init_null_args)
{
	a02yyuw_t dev;
	zassert_equal(a02yyuw_init(NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(a02yyuw_init(&dev, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_drv8833_init_null_args)
{
	drv8833_t dev;
	zassert_equal(drv8833_init(NULL, NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(drv8833_init(&dev, NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_drv8825_init_null_args)
{
	drv8825_t dev;
	zassert_equal(drv8825_init(NULL, NULL, NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(drv8825_init(&dev, NULL, NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_tmc2209_init_null_args)
{
	tmc2209_t dev;
	zassert_equal(tmc2209_init(NULL, NULL, 0), ALP_ERR_INVAL);
	zassert_equal(tmc2209_init(&dev, NULL, 0), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_a4988_init_null_args)
{
	a4988_t dev;
	zassert_equal(a4988_init(NULL, NULL, NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(a4988_init(&dev, NULL, NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_as5048b_init_null_args)
{
	as5048b_t  dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(as5048b_init(NULL, bus, AS5048B_I2C_ADDR_BASE), ALP_ERR_INVAL);
	zassert_equal(as5048b_init(&dev, NULL, AS5048B_I2C_ADDR_BASE), ALP_ERR_INVAL);
	zassert_equal(as5048b_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_mt6701_init_null_args)
{
	mt6701_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(mt6701_init(NULL, bus, MT6701_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(mt6701_init(&dev, NULL, MT6701_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(mt6701_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_hx711_init_null_args)
{
	hx711_t dev;
	zassert_equal(hx711_init(NULL, NULL, NULL, HX711_GAIN_128), ALP_ERR_INVAL);
	zassert_equal(hx711_init(&dev, NULL, NULL, HX711_GAIN_128), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_max31855_init_null_args)
{
	max31855_t dev;
	zassert_equal(max31855_init(NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(max31855_init(&dev, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_max31865_init_null_args)
{
	max31865_t dev;
	zassert_equal(max31865_init(NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(max31865_init(&dev, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_tsl2591_init_null_args)
{
	tsl2591_t  dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(tsl2591_init(NULL, bus, TSL2591_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(tsl2591_init(&dev, NULL, TSL2591_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(tsl2591_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_qmc5883l_init_null_args)
{
	qmc5883l_t dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(qmc5883l_init(NULL, bus, QMC5883L_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(qmc5883l_init(&dev, NULL, QMC5883L_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(qmc5883l_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_veml7700_init_null_args)
{
	veml7700_t dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(veml7700_init(NULL, bus, VEML7700_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(veml7700_init(&dev, NULL, VEML7700_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(veml7700_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}
