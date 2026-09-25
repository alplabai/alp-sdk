/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/wdt.h> -- watchdog wrapper tests.  §C.16 split + §C.22
 * thin-spot fills: NULL-handle guards on feed / disable / close
 * so the binding layer's input-validation contract is exercised
 * on every native_sim build.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/wdt.h"

ZTEST(alp_peripheral, test_wdt_null_cfg)
{
	zassert_is_null(alp_wdt_open(NULL));
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_wdt_zero_timeout_rejected)
{
	alp_wdt_t *w = alp_wdt_open(
	    &(alp_wdt_config_t){ .wdt_id = 0, .timeout_ms = 0, .on_timeout = ALP_WDT_RESET_SOC });
	zassert_is_null(w);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_wdt_out_of_range_id_rejected)
{
	/* §C.22: wdt_id beyond the wrapper's pool size rejects. */
	alp_wdt_t *w = alp_wdt_open(
	    &(alp_wdt_config_t){ .wdt_id = 99, .timeout_ms = 1000u, .on_timeout = ALP_WDT_RESET_SOC });
	zassert_is_null(w);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

static void _unused_expiry_cb(alp_wdt_t *wdt, void *user)
{
	ARG_UNUSED(wdt);
	ARG_UNUSED(user);
}

ZTEST(alp_peripheral, test_wdt_interrupt_only_without_callback_rejected)
{
	/* #1637: ALP_WDT_INTERRUPT_ONLY used to be accepted with no way to
     * observe the interrupt -- a watchdog that neither resets nor
     * notifies anyone.  This check runs in the dispatcher before any
     * backend is consulted, so it is exercised here regardless of
     * which backend this build selects. */
	alp_wdt_t *w = alp_wdt_open(&(alp_wdt_config_t){ .wdt_id     = 0,
	                                                 .timeout_ms = 1000u,
	                                                 .on_timeout = ALP_WDT_INTERRUPT_ONLY,
	                                                 .on_expire  = NULL });
	zassert_is_null(w, "INTERRUPT_ONLY with no on_expire must be refused");
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_wdt_interrupt_only_with_callback_passes_validation)
{
	/* The same request WITH a callback must clear the dispatcher's
     * validation -- this build's zephyr_drv backend then fails with
     * NOT_READY (no alp-wdt0 DT alias on native_sim, per this suite's
     * prj.conf comment), which is enough to prove the INVAL check
     * above is not simply rejecting every INTERRUPT_ONLY request. */
	alp_wdt_t *w = alp_wdt_open(&(alp_wdt_config_t){ .wdt_id     = 0,
	                                                 .timeout_ms = 1000u,
	                                                 .on_timeout = ALP_WDT_INTERRUPT_ONLY,
	                                                 .on_expire  = _unused_expiry_cb });
	zassert_is_null(w);
	zassert_not_equal(alp_last_error(),
	                  ALP_ERR_INVAL,
	                  "a well-formed INTERRUPT_ONLY request must not fail dispatcher validation");
}

ZTEST(alp_peripheral, test_wdt_feed_null_handle_not_ready)
{
	/* §C.22: feeding a closed / NULL watchdog should fail safely;
     * a regression that silently no-ops would mask a stuck
     * watchdog in production. */
	zassert_equal(alp_wdt_feed(NULL), ALP_ERR_NOT_READY);
}

ZTEST(alp_peripheral, test_wdt_disable_null_handle_not_ready)
{
	zassert_equal(alp_wdt_disable(NULL), ALP_ERR_NOT_READY);
}
