/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/wdt.h> -- watchdog wrapper tests.  Extracted from main.c in
 * §C.16.
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
