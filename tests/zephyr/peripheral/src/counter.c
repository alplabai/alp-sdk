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

/*
 * The alp-sdk#1242 invariant: a PUBLISHED E1M-X connector identity this
 * board does not serve never reports ALP_ERR_INVAL.  INVAL reads as "you
 * passed something malformed" and sends the caller hunting a bug in
 * their own code, when the truth is that this SoM serves fewer counters.
 *
 * Which non-INVAL status appears is deliberately NOT asserted, because it
 * is backend-dependent and both answers are correct:
 *   - with the V2N/V2M supervisor built in, the GD32 bridge is selected
 *     and reports ALP_ERR_NOSUPPORT (it serves ALP_E1M_X_COUNTER0 only);
 *   - otherwise the generic Zephyr backend is selected, whose
 *     `alp-counter<N>` table is a fixed four entries, so a published id
 *     with no alias reports ALP_ERR_NOT_READY.
 * Pinning either one would make this test pass in one twister scenario
 * and fail in the other, which is how it was first written and caught.
 */
ZTEST(alp_peripheral, test_counter_unserved_published_id_is_never_inval)
{
	const alp_counter_config_t cfg = { .counter_id = 3u };

	zassert_is_null(alp_counter_open(&cfg));
	zassert_not_equal(
	    alp_last_error(), ALP_ERR_INVAL, "a published connector id must not report INVAL");
	zassert_true(alp_last_error() == ALP_ERR_NOSUPPORT || alp_last_error() == ALP_ERR_NOT_READY,
	             "expected NOSUPPORT (bridge) or NOT_READY (zephyr), got %d",
	             (int)alp_last_error());
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
