/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/counter.h> -- counter wrapper tests.  §C.16 split + §C.22
 * thin-spot fills: every public function on the counter surface
 * gets a NULL-handle / NULL-arg guard test so the binding-layer
 * contract is exercised on every native_sim build.
 */

#include <zephyr/ztest.h>

#include "alp/counter.h"
#include "alp/peripheral.h"

ZTEST(alp_peripheral, test_counter_null_cfg)
{
	zassert_is_null(alp_counter_open(NULL));
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_counter_start_null_handle_not_ready)
{
	zassert_equal(alp_counter_start(NULL), ALP_ERR_NOT_READY);
}

ZTEST(alp_peripheral, test_counter_stop_null_handle_not_ready)
{
	zassert_equal(alp_counter_stop(NULL), ALP_ERR_NOT_READY);
}

ZTEST(alp_peripheral, test_counter_get_value_null_handle_not_ready)
{
	uint32_t ticks = 99u;
	zassert_equal(alp_counter_get_value(NULL, &ticks), ALP_ERR_NOT_READY);
}

ZTEST(alp_peripheral, test_counter_get_value_null_out_invalid)
{
	/* NULL out-param is INVAL even when handle is NULL -- the
     * argument-validation pass runs before the handle check. */
	zassert_equal(alp_counter_get_value(NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_counter_us_to_ticks_null_handle_not_ready)
{
	uint32_t ticks = 99u;
	zassert_equal(alp_counter_us_to_ticks(NULL, 1000u, &ticks), ALP_ERR_NOT_READY);
}

ZTEST(alp_peripheral, test_counter_cancel_alarm_null_handle_not_ready)
{
	zassert_equal(alp_counter_cancel_alarm(NULL), ALP_ERR_NOT_READY);
}

ZTEST(alp_peripheral, test_counter_close_null_is_noop)
{
	/* close(NULL) is a documented no-op; the test just guards
     * against regressions that crash on NULL. */
	alp_counter_close(NULL);
}
