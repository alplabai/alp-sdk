/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/i2s.h> -- I2S wrapper tests.  Extracted from main.c in §C.16.
 */

#include <zephyr/ztest.h>

#include "alp/i2s.h"
#include "alp/peripheral.h"

ZTEST(alp_peripheral, test_i2s_null_cfg) {
    zassert_is_null(alp_i2s_open(NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_i2s_invalid_channels_rejected) {
    alp_i2s_t *i = alp_i2s_open(&(alp_i2s_config_t){
        .bus_id = 0, .sample_rate_hz = 16000,
        .word_bits = 16, .channels = 5,         /* > 2 → INVAL */
        .block_frames = 64});
    zassert_is_null(i);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}
