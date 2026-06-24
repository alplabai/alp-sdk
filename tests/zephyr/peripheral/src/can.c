/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/can.h> -- CAN wrapper tests.  Extracted from main.c in §C.16.
 */

#include <zephyr/ztest.h>

#include "alp/can.h"
#include "alp/peripheral.h"

ZTEST(alp_peripheral, test_can_null_cfg)
{
	zassert_is_null(alp_can_open(NULL));
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_can_zero_bitrate_rejected)
{
	alp_can_t *c = alp_can_open(&(alp_can_config_t){ .bus_id             = 0,
	                                                 .bitrate_nominal_hz = 0, /* INVAL */
	                                                 .mode               = ALP_CAN_MODE_CLASSIC });
	zassert_is_null(c);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}
