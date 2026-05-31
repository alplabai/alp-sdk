/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/i2s.h> -- I2S wrapper tests.  §C.16 split + §C.22 thin-spot
 * fills: NULL-handle guards on every public function on the
 * surface so the binding layer's input-validation contract is
 * exercised on every native_sim build.
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

ZTEST(alp_peripheral, test_i2s_invalid_word_bits_rejected)
{
    /* §C.22: word_bits must be 8 / 16 / 24 / 32; anything else
     * rejects with INVAL at open time. */
    alp_i2s_t *i = alp_i2s_open(&(alp_i2s_config_t){
        .bus_id         = 0,
        .sample_rate_hz = 16000,
        .word_bits      = 7,    /* not 8/16/24/32 */
        .channels       = 2,
        .block_frames   = 64,
    });
    zassert_is_null(i);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_i2s_zero_block_frames_rejected)
{
    /* block_frames == 0 makes no sense; reject at open. */
    alp_i2s_t *i = alp_i2s_open(&(alp_i2s_config_t){
        .bus_id         = 0,
        .sample_rate_hz = 16000,
        .word_bits      = 16,
        .channels       = 2,
        .block_frames   = 0,
    });
    zassert_is_null(i);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}
