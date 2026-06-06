/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke tests for the <alp/audio.h> wrapper under native_sim.
 *
 * The host build doesn't enable AUDIO_DMIC or I2S so the wrapper's
 * "no audio backend present" branch should fall back to the v0.1
 * NULL-with-NOSUPPORT contract.  Real DMIC + I2S round-trips run
 * under HW-in-loop CI.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/audio.h"

ZTEST_SUITE(alp_audio, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------ */
/* Audio input                                                         */
/* ------------------------------------------------------------------ */

ZTEST(alp_audio, test_in_open_no_backend_returns_null)
{
    alp_audio_in_t *h = alp_audio_in_open(&(alp_audio_config_t){
        .peripheral_id    = 0,
        .sample_rate_hz   = 16000,
        .channels         = 1,
        .format           = ALP_AUDIO_FMT_S16_LE,
        .frames_per_block = 256,
    });
    zassert_is_null(h, "alp_audio_in_open without DMIC must yield NULL");
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT, "expected NOSUPPORT, got %d",
                  (int)alp_last_error());
}

ZTEST(alp_audio, test_in_open_null_cfg_invalid)
{
    alp_audio_in_t *h = alp_audio_in_open(NULL);
    zassert_is_null(h);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_audio, test_in_open_zero_frames_invalid)
{
    alp_audio_in_t *h = alp_audio_in_open(&(alp_audio_config_t){
        .peripheral_id    = 0,
        .sample_rate_hz   = 16000,
        .channels         = 1,
        .format           = ALP_AUDIO_FMT_S16_LE,
        .frames_per_block = 0,
    });
    zassert_is_null(h);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_audio, test_in_open_too_many_channels_invalid)
{
    alp_audio_in_t *h = alp_audio_in_open(&(alp_audio_config_t){
        .channels         = 3,
        .sample_rate_hz   = 16000,
        .format           = ALP_AUDIO_FMT_S16_LE,
        .frames_per_block = 256,
    });
    zassert_is_null(h);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_audio, test_in_lifecycle_null_handle_safe)
{
    zassert_equal(alp_audio_in_start(NULL), ALP_ERR_NOT_READY);
    zassert_equal(alp_audio_in_stop(NULL), ALP_ERR_NOT_READY);
    size_t  got = 999;
    int16_t buf[16];
    zassert_equal(alp_audio_in_read(NULL, buf, 8, &got, 100), ALP_ERR_NOT_READY);
    zassert_equal(got, 0, "out_frames must be cleared on early-return");
    alp_audio_in_close(NULL);
}

/* ------------------------------------------------------------------ */
/* Audio output                                                        */
/* ------------------------------------------------------------------ */

ZTEST(alp_audio, test_out_open_no_backend_returns_null)
{
    alp_audio_out_t *h = alp_audio_out_open(&(alp_audio_config_t){
        .peripheral_id    = 0,
        .sample_rate_hz   = 48000,
        .channels         = 2,
        .format           = ALP_AUDIO_FMT_S16_LE,
        .frames_per_block = 256,
    });
    zassert_is_null(h, "alp_audio_out_open without I2S must yield NULL");
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT, "expected NOSUPPORT, got %d",
                  (int)alp_last_error());
}

ZTEST(alp_audio, test_out_open_null_cfg_invalid)
{
    zassert_is_null(alp_audio_out_open(NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_audio, test_out_lifecycle_null_handle_safe)
{
    zassert_equal(alp_audio_out_start(NULL), ALP_ERR_NOT_READY);
    zassert_equal(alp_audio_out_stop(NULL), ALP_ERR_NOT_READY);
    int16_t pcm[8] = {0};
    size_t  got    = 999;
    zassert_equal(alp_audio_out_write(NULL, pcm, 4, &got, 100), ALP_ERR_NOT_READY);
    zassert_equal(got, 0);
    zassert_equal(alp_audio_out_set_volume(NULL, 128), ALP_ERR_NOT_READY);
    alp_audio_out_close(NULL);
}
