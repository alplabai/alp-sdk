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
 * alp-sdk#1242 first held that a PUBLISHED E1M-X connector identity this
 * board does not serve must never report ALP_ERR_INVAL, on the theory
 * that INVAL reads as "you passed something malformed" and sends the
 * caller hunting a bug in their own code.  alp-sdk#1635 overrode that for
 * the GD32 bridge specifically, for consistency with its adc / dac / pwm
 * siblings (src/backends/{adc,dac,pwm}/gd32_bridge.c): "you asked for an
 * instance that does not exist" is one question with one answer across
 * this SoM vendor's SDK, and NOSUPPORT is reserved for the different
 * question "this build cannot do that at all".  So the bridge now
 * reports ALP_ERR_INVAL for counter_id=3 (it serves ALP_E1M_X_COUNTER0
 * only) -- this is the runnable check that fails if gd32_bridge.c ever
 * reverts to NOSUPPORT.
 *
 * The generic Zephyr backend (no V2N/V2M supervisor) is untouched by
 * #1635: its `alp-counter<N>` table is a fixed four entries, so id=3 is
 * in-range but has no alias on this test overlay, giving NOT_READY.
 */
ZTEST(alp_peripheral, test_counter_unserved_published_id_status)
{
	const alp_counter_config_t cfg = { .counter_id = 3u };

	zassert_is_null(alp_counter_open(&cfg));
#if defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)
	zassert_equal(alp_last_error(),
	              ALP_ERR_INVAL,
	              "gd32_bridge must report INVAL for an out-of-range counter id (alp-sdk#1635), "
	              "got %d",
	              (int)alp_last_error());
#else
	zassert_equal(alp_last_error(),
	              ALP_ERR_NOT_READY,
	              "generic zephyr backend must report NOT_READY for an unaliased id, got %d",
	              (int)alp_last_error());
#endif
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
