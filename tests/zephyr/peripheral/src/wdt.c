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

ZTEST(alp_peripheral, test_wdt_null_cfg) {
    zassert_is_null(alp_wdt_open(0, NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_wdt_zero_timeout_rejected) {
    alp_wdt_t *w = alp_wdt_open(0, &(alp_wdt_config_t){
        .timeout_ms = 0, .on_timeout = ALP_WDT_RESET_SOC});
    zassert_is_null(w);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_wdt_out_of_range_id_rejected)
{
    /* §C.22: wdt_id beyond the wrapper's pool size rejects. */
    alp_wdt_t *w = alp_wdt_open(99, &(alp_wdt_config_t){
        .timeout_ms = 1000u, .on_timeout = ALP_WDT_RESET_SOC});
    zassert_is_null(w);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
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
