/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/rtc.h> -- RTC wrapper tests.  Extracted from main.c in §C.16.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/rtc.h"

ZTEST(alp_peripheral, test_rtc_out_of_range_id) {
    /* rtc_id = 99 exceeds the wrapper's hard array bound (2). */
    zassert_is_null(alp_rtc_open(99));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}
