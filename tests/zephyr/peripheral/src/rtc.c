/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/rtc.h> -- RTC wrapper tests.  §C.16 split + §C.22 thin-spot
 * fills: NULL-handle propagation for set_time / get_time, and a
 * lifecycle round-trip against the stub backend.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/rtc.h"

ZTEST(alp_peripheral, test_rtc_out_of_range_id)
{
	/* rtc_id = 99 exceeds the wrapper's hard array bound (2). */
	zassert_is_null(alp_rtc_open(99));
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_rtc_set_time_null_handle_not_ready)
{
	/* §C.22 thin-spot fill.  Public-surface NULL-handle guard
     * regardless of whether a real RTC backend is wired. */
	alp_rtc_time_t t = { 0 };
	zassert_equal(alp_rtc_set_time(NULL, &t), ALP_ERR_NOT_READY);
}

ZTEST(alp_peripheral, test_rtc_set_time_null_time_invalid)
{
	/* NULL `time` is INVAL even on a NULL handle -- the wrapper
     * validates arguments before touching the handle. */
	zassert_equal(alp_rtc_set_time(NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_rtc_get_time_null_handle_not_ready)
{
	alp_rtc_time_t t = { 0 };
	zassert_equal(alp_rtc_get_time(NULL, &t), ALP_ERR_NOT_READY);
}

ZTEST(alp_peripheral, test_rtc_get_time_null_out_invalid)
{
	/* NULL `time` is INVAL -- caller needs the output. */
	zassert_equal(alp_rtc_get_time(NULL, NULL), ALP_ERR_INVAL);
}
