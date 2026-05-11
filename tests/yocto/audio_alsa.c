/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain-CMake tests for the Yocto/ALSA audio backend
 * (src/yocto/audio_yocto.c).
 *
 * Failure-path coverage only -- happy paths require a working
 * ALSA configuration on the runner (real device or dummy driver),
 * which is parked behind ci/HW-IN-LOOP.md.  The tests below
 * exercise argument validation + the resolve_device_name +
 * configure_pcm error paths.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_audio_alsa
 *   ctest --test-dir build -R alp_test_audio_alsa
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/audio.h"
#include "alp/peripheral.h"

#include "test_assert.h"

/* CI runners don't generally have ALSA devices wired up; the
 * default "default" device exists in /etc/asound.conf form on
 * ubuntu-latest but PCM-open against it fails cleanly to NOT_READY
 * or IO depending on the runner.  We assert "open refuses" rather
 * than a specific status code so the test passes on any
 * deviceless host. */

static void test_in_null_cfg_returns_null(void)
{
    alp_audio_in_t *h = alp_audio_in_open(NULL);
    ALP_ASSERT_NULL(h);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_out_null_cfg_returns_null(void)
{
    alp_audio_out_t *h = alp_audio_out_open(NULL);
    ALP_ASSERT_NULL(h);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_in_invalid_format_returns_null(void)
{
    alp_audio_config_t cfg = {
        .peripheral_id    = 0,
        .sample_rate_hz   = 16000,
        .channels         = 1,
        .format           = (alp_audio_format_t)99,  /* not a valid format */
        .frames_per_block = 256,
    };
    alp_audio_in_t *h = alp_audio_in_open(&cfg);
    ALP_ASSERT_NULL(h);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_out_invalid_format_returns_null(void)
{
    alp_audio_config_t cfg = {
        .peripheral_id    = 0,
        .sample_rate_hz   = 48000,
        .channels         = 2,
        .format           = (alp_audio_format_t)99,
        .frames_per_block = 256,
    };
    alp_audio_out_t *h = alp_audio_out_open(&cfg);
    ALP_ASSERT_NULL(h);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_in_unreachable_device_refuses(void)
{
    /* peripheral_id = 999 => "hw:998,0", which never exists. ALSA
     * returns -ENOENT / -ENODEV; we map to NOT_READY. */
    alp_audio_config_t cfg = {
        .peripheral_id    = 999,
        .sample_rate_hz   = 16000,
        .channels         = 1,
        .format           = ALP_AUDIO_FMT_S16_LE,
        .frames_per_block = 256,
    };
    alp_audio_in_t *h = alp_audio_in_open(&cfg);
    ALP_ASSERT_NULL(h);
    /* Either NOT_READY (device missing) or IO (driver failure) is
     * acceptable -- we just assert "open refused". */
    ALP_ASSERT_TRUE(alp_last_error() != ALP_OK);
}

static void test_start_on_null_returns_not_ready(void)
{
    ALP_ASSERT_EQ_INT(alp_audio_in_start(NULL), ALP_ERR_NOT_READY);
    ALP_ASSERT_EQ_INT(alp_audio_out_start(NULL), ALP_ERR_NOT_READY);
}

static void test_stop_on_null_returns_not_ready(void)
{
    ALP_ASSERT_EQ_INT(alp_audio_in_stop(NULL), ALP_ERR_NOT_READY);
    ALP_ASSERT_EQ_INT(alp_audio_out_stop(NULL), ALP_ERR_NOT_READY);
}

static void test_read_on_null_returns_not_ready(void)
{
    uint8_t buf[64];
    size_t  got = 99;
    alp_status_t s = alp_audio_in_read(NULL, buf, sizeof(buf), &got, 100);
    ALP_ASSERT_EQ_INT(s, ALP_ERR_NOT_READY);
    ALP_ASSERT_EQ_INT(got, 0);
}

static void test_write_on_null_returns_not_ready(void)
{
    const uint8_t payload[] = "abcdef";
    size_t        wrote     = 99;
    alp_status_t  s         = alp_audio_out_write(NULL, payload, sizeof(payload), &wrote, 100);
    ALP_ASSERT_EQ_INT(s, ALP_ERR_NOT_READY);
    ALP_ASSERT_EQ_INT(wrote, 0);
}

static void test_set_volume_on_null_returns_not_ready(void)
{
    ALP_ASSERT_EQ_INT(alp_audio_out_set_volume(NULL, 128), ALP_ERR_NOT_READY);
}

static void test_close_null_is_safe(void)
{
    alp_audio_in_close(NULL);
    alp_audio_out_close(NULL);
    ALP_TEST_PASS();
}

int main(void)
{
    test_in_null_cfg_returns_null();
    test_out_null_cfg_returns_null();
    test_in_invalid_format_returns_null();
    test_out_invalid_format_returns_null();
    test_in_unreachable_device_refuses();
    test_start_on_null_returns_not_ready();
    test_stop_on_null_returns_not_ready();
    test_read_on_null_returns_not_ready();
    test_write_on_null_returns_not_ready();
    test_set_volume_on_null_returns_not_ready();
    test_close_null_is_safe();

    ALP_TEST_SUMMARY();
}
