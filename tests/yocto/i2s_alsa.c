/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain-CMake tests for the Yocto/ALSA I²S backend
 * (src/backends/i2s/yocto_drv.c).
 *
 * Failure-path coverage only -- happy paths (a supported-rate open plus
 * TX/RX round-trip) require a real I²S DAI, which is parked behind
 * docs/ci/HW-IN-LOOP.md same as the sibling audio backend.  The tests
 * below exercise argument validation and the unreachable-device path,
 * mirroring tests/yocto/audio_alsa.c's non-DAI coverage -- these paths
 * don't need a real ALSA device to observe (#1648 tier 1).
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_i2s_alsa
 *   ctest --test-dir build -R alp_test_i2s_alsa
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/i2s.h"
#include "alp/peripheral.h"

#include "test_assert.h"

static void test_open_null_cfg_returns_null(void)
{
	alp_i2s_t *h = alp_i2s_open(NULL);
	ALP_ASSERT_NULL(h);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_open_zero_channels_returns_null(void)
{
	alp_i2s_config_t cfg = ALP_I2S_CONFIG_DEFAULT(0);
	cfg.channels          = 0;
	alp_i2s_t *h          = alp_i2s_open(&cfg);
	ALP_ASSERT_NULL(h);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_open_too_many_channels_returns_null(void)
{
	/* Backend caps at stereo (2); the ALSA hwparams path has no mono/
     * stereo-only assumption baked into the public header, but
     * y_open() itself refuses > 2. */
	alp_i2s_config_t cfg = ALP_I2S_CONFIG_DEFAULT(0);
	cfg.channels          = 3;
	alp_i2s_t *h          = alp_i2s_open(&cfg);
	ALP_ASSERT_NULL(h);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_open_zero_sample_rate_returns_null(void)
{
	alp_i2s_config_t cfg = ALP_I2S_CONFIG_DEFAULT(0);
	cfg.sample_rate_hz    = 0;
	alp_i2s_t *h          = alp_i2s_open(&cfg);
	ALP_ASSERT_NULL(h);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_open_zero_block_frames_returns_null(void)
{
	alp_i2s_config_t cfg = ALP_I2S_CONFIG_DEFAULT(0);
	cfg.block_frames      = 0;
	alp_i2s_t *h          = alp_i2s_open(&cfg);
	ALP_ASSERT_NULL(h);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_open_non_i2s_format_returns_nosupport(void)
{
	/* Only ALP_I2S_FMT_I2S maps to a userspace ALSA PCM setting; every
     * other wire format is a DAI-link property (see yocto_drv.c's
     * y_open() doc comment). */
	alp_i2s_config_t cfg = ALP_I2S_CONFIG_DEFAULT(0);
	cfg.format             = ALP_I2S_FMT_LEFT_JUSTIFIED;
	alp_i2s_t *h            = alp_i2s_open(&cfg);
	ALP_ASSERT_NULL(h);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_NOSUPPORT);
}

static void test_open_full_duplex_returns_nosupport(void)
{
	alp_i2s_config_t cfg = ALP_I2S_CONFIG_DEFAULT(0);
	cfg.direction          = ALP_I2S_DIR_BOTH;
	alp_i2s_t *h            = alp_i2s_open(&cfg);
	ALP_ASSERT_NULL(h);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_NOSUPPORT);
}

static void test_open_unreachable_device_refuses(void)
{
	/* bus_id = 999 -> "hw:998,0", which never exists.  ALSA returns
     * -ENOENT/-ENODEV; the backend maps that to NOT_READY or IO.  We
     * only assert "open refused", same tolerance as the sibling audio
     * test, so this passes on any deviceless host. */
	alp_i2s_config_t cfg = ALP_I2S_CONFIG_DEFAULT(999);
	alp_i2s_t *h          = alp_i2s_open(&cfg);
	ALP_ASSERT_NULL(h);
	ALP_ASSERT_TRUE(alp_last_error() != ALP_OK);
}

static void test_start_on_null_returns_not_ready(void)
{
	ALP_ASSERT_EQ_INT(alp_i2s_start(NULL), ALP_ERR_NOT_READY);
}

static void test_stop_on_null_returns_not_ready(void)
{
	ALP_ASSERT_EQ_INT(alp_i2s_stop(NULL), ALP_ERR_NOT_READY);
}

static void test_write_on_null_returns_not_ready(void)
{
	const uint8_t payload[] = "abcdef";
	alp_status_t  s         = alp_i2s_write(NULL, payload, sizeof(payload), 100);
	ALP_ASSERT_EQ_INT(s, ALP_ERR_NOT_READY);
}

static void test_read_on_null_returns_not_ready(void)
{
	uint8_t      buf[64];
	size_t       got = 99;
	alp_status_t s   = alp_i2s_read(NULL, buf, sizeof(buf), &got, 100);
	ALP_ASSERT_EQ_INT(s, ALP_ERR_NOT_READY);
	ALP_ASSERT_EQ_INT(got, 0);
}

static void test_capabilities_on_null_returns_null(void)
{
	ALP_ASSERT_NULL(alp_i2s_capabilities(NULL));
}

static void test_close_null_is_safe(void)
{
	alp_i2s_close(NULL);
	ALP_TEST_PASS();
}

int main(void)
{
	test_open_null_cfg_returns_null();
	test_open_zero_channels_returns_null();
	test_open_too_many_channels_returns_null();
	test_open_zero_sample_rate_returns_null();
	test_open_zero_block_frames_returns_null();
	test_open_non_i2s_format_returns_nosupport();
	test_open_full_duplex_returns_nosupport();
	test_open_unreachable_device_refuses();
	test_start_on_null_returns_not_ready();
	test_stop_on_null_returns_not_ready();
	test_write_on_null_returns_not_ready();
	test_read_on_null_returns_not_ready();
	test_capabilities_on_null_returns_null();
	test_close_null_is_safe();

	ALP_TEST_SUMMARY();
}
