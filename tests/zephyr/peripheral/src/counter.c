/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/counter.h> -- counter wrapper NULL-cfg test.  Extracted from
 * main.c in §C.16.  Bigger surface (counter_id range, alarm setup,
 * read deltas) lands as HW-gated tests in a follow-up.
 */

#include <zephyr/ztest.h>

#include "alp/counter.h"
#include "alp/peripheral.h"

ZTEST(alp_peripheral, test_counter_null_cfg) {
    zassert_is_null(alp_counter_open(NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}
