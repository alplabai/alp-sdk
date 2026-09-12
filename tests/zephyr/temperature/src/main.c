/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke tests for <alp/temperature.h> under native_sim.  No `alp-temp0`
 * alias and no CONFIG_SENSOR on this target, so
 * alp_temperature_read_milli_c() must take the documented NOSUPPORT
 * branch -- and, unlike <alp/hw_info.h>'s zero-fill-on-failure contract,
 * this API promises the out-param is left UNTOUCHED on any error, which
 * these tests prove with a sentinel value.
 */

#include <stdint.h>

#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/temperature.h"

ZTEST_SUITE(alp_temperature, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_temperature, test_read_null_out_returns_inval)
{
	zassert_equal(alp_temperature_read_milli_c(NULL), ALP_ERR_INVAL);
}

ZTEST(alp_temperature, test_read_returns_nosupport_with_no_on_module_sensor)
{
	int32_t milli_c = 0x7EADBEEF; /* sentinel */

	zassert_equal(alp_temperature_read_milli_c(&milli_c), ALP_ERR_NOSUPPORT);

	/* Contract is "left untouched on any error" -- NOT zero-filled like
	 * alp_hw_info_read()'s out-struct; prove the sentinel survives. */
	zassert_equal(milli_c, 0x7EADBEEF);
}
