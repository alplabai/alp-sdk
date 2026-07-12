/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Camera / vision-sensor chip smokes: ov5640, cam_mux_pi3wvr626, and the
 * v0.5 §D.AI batch image sensors + GMSL/FPD-Link serializer-deserializer
 * pairs -- lifecycle + NULL-arg validation.
 */

#include <zephyr/ztest.h>

#include "alp/chips/ar0234.h"
#include "alp/chips/cam_mux_pi3wvr626.h"
#include "alp/chips/gc2145.h"
#include "alp/chips/imx219.h"
#include "alp/chips/imx477.h"
#include "alp/chips/maxim_max9295_9296.h"
#include "alp/chips/ov2640.h"
#include "alp/chips/ov5640.h"
#include "alp/chips/ov5645.h"
#include "alp/chips/ov7670.h"
#include "alp/chips/ov9281.h"
#include "alp/chips/ti_ds90ub953_954.h"
#include "alp/e1m_pinout.h"
#include "alp/peripheral.h"

/* ------------------------------------------------------------------ */
/* ov5640 (v0.2 chip — SCCB config side)                               */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_ov5640_init_null_args)
{
	ov5640_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	zassert_equal(ov5640_init(NULL, bus, OV5640_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(ov5640_init(&dev, NULL, OV5640_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(ov5640_init(&dev, bus, 0), ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ov5640_post_init_calls_reject_uninitialised)
{
	ov5640_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	alp_status_t s = ov5640_init(&dev, bus, OV5640_I2C_ADDR);
	if (s != ALP_OK) {
		zassert_equal(ov5640_set_resolution(&dev, OV5640_RES_VGA), ALP_ERR_NOT_READY);
		zassert_equal(ov5640_set_format(&dev, OV5640_FMT_RGB565), ALP_ERR_NOT_READY);
		zassert_equal(ov5640_set_test_pattern(&dev, true), ALP_ERR_NOT_READY);
	}

	ov5640_deinit(&dev);
	alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* cam_mux_pi3wvr626 -- MIPI CSI 2:1 mux (GPIO-only)                  */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_cam_mux_pi3wvr626_init_null_args)
{
	cam_mux_pi3wvr626_t ctx;
	alp_gpio_t         *bogus = (alp_gpio_t *)0xDEADBEEFu;

	zassert_equal(cam_mux_pi3wvr626_init(NULL, bogus), ALP_ERR_INVAL);
	zassert_equal(cam_mux_pi3wvr626_init(&ctx, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_cam_mux_pi3wvr626_calls_reject_uninitialised)
{
	cam_mux_pi3wvr626_t       ctx = { 0 };
	cam_mux_pi3wvr626_input_t got;

	zassert_equal(cam_mux_pi3wvr626_select(&ctx, (cam_mux_pi3wvr626_input_t)0), ALP_ERR_NOT_READY);
	zassert_equal(cam_mux_pi3wvr626_get(&ctx, &got), ALP_ERR_NOT_READY);
}

/* ------------------------------------------------------------------ */
/* v0.5 §D.AI batch -- image sensors + serializer/deserializer pairs  */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_ov2640_init_null_args)
{
	ov2640_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(ov2640_init(NULL, bus, OV2640_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(ov2640_init(&dev, NULL, OV2640_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(ov2640_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ov5645_init_null_args)
{
	ov5645_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(ov5645_init(NULL, bus, OV5645_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(ov5645_init(&dev, NULL, OV5645_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(ov5645_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ov7670_init_null_args)
{
	ov7670_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(ov7670_init(NULL, bus, OV7670_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(ov7670_init(&dev, NULL, OV7670_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(ov7670_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ov9281_init_null_args)
{
	ov9281_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(ov9281_init(NULL, bus, OV9281_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(ov9281_init(&dev, NULL, OV9281_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(ov9281_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ar0234_init_null_args)
{
	ar0234_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(ar0234_init(NULL, bus, AR0234_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(ar0234_init(&dev, NULL, AR0234_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(ar0234_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_imx219_init_null_args)
{
	imx219_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(imx219_init(NULL, bus, IMX219_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(imx219_init(&dev, NULL, IMX219_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(imx219_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_imx477_init_null_args)
{
	imx477_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(imx477_init(NULL, bus, IMX477_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(imx477_init(&dev, NULL, IMX477_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(imx477_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_gc2145_init_null_args)
{
	gc2145_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(gc2145_init(NULL, bus, GC2145_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(gc2145_init(&dev, NULL, GC2145_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(gc2145_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ti_ds90ub_init_null_args)
{
	ti_ds90ub_t dev;
	alp_i2c_t  *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(ti_ds90ub_init(NULL, bus, DS90UB953_I2C_ADDR_DEFAULT, DS90UB954_I2C_ADDR_DEFAULT),
	              ALP_ERR_INVAL);
	zassert_equal(
	    ti_ds90ub_init(&dev, NULL, DS90UB953_I2C_ADDR_DEFAULT, DS90UB954_I2C_ADDR_DEFAULT),
	    ALP_ERR_INVAL);
	zassert_equal(ti_ds90ub_init(&dev, bus, 0, DS90UB954_I2C_ADDR_DEFAULT), ALP_ERR_INVAL);
	zassert_equal(ti_ds90ub_init(&dev, bus, DS90UB953_I2C_ADDR_DEFAULT, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_maxim_gmsl2_init_null_args)
{
	maxim_gmsl2_t dev;
	alp_i2c_t    *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(maxim_gmsl2_init(NULL, bus, MAX9295_I2C_ADDR_DEFAULT, MAX9296_I2C_ADDR_DEFAULT),
	              ALP_ERR_INVAL);
	zassert_equal(maxim_gmsl2_init(&dev, NULL, MAX9295_I2C_ADDR_DEFAULT, MAX9296_I2C_ADDR_DEFAULT),
	              ALP_ERR_INVAL);
	zassert_equal(maxim_gmsl2_init(&dev, bus, 0, MAX9296_I2C_ADDR_DEFAULT), ALP_ERR_INVAL);
	zassert_equal(maxim_gmsl2_init(&dev, bus, MAX9295_I2C_ADDR_DEFAULT, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}
