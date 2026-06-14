/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/peripheral.h> -- DAC wrapper tests.  Extracted from main.c in
 * §C.16.  Covers NULL-cfg INVAL, out-of-range channel, and the
 * NOT_READY-or-NOSUPPORT path when no underlying DAC controller or
 * V2N supervisor is wired.
 */

#include <zephyr/ztest.h>

#include "alp/dac.h" /* alp_dac_open / alp_dac_t / alp_dac_config_t */
#include "alp/peripheral.h"

ZTEST(alp_peripheral, test_dac_null_cfg)
{
    zassert_is_null(alp_dac_open(NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_dac_out_of_range_channel)
{
    /* E1M_DAC_COUNT = 2; the wrapper's internal array sized to
     * match.  Channel id 9 must reject. */
    alp_dac_t *d = alp_dac_open(&(alp_dac_config_t){
        .channel_id = 9u,
        .initial_mv = 0u,
    });
    zassert_is_null(d);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_dac_unresolved_channel_yields_not_ready)
{
    /* Without a real DAC controller or V2N supervisor backing
     * channel 0, open must fail with NOT_READY (DT-alias path) or
     * NOSUPPORT (CONFIG_DAC=n).  Either is acceptable; both surface
     * as a NULL return. */
    alp_dac_t *d = alp_dac_open(&(alp_dac_config_t){
        .channel_id = 0u,
        .initial_mv = 0u,
    });
    zassert_is_null(d);
}
