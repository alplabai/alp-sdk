/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #2134: when the PDM refuses the requested rate, src/backends/audio/
 * zephyr_drv.c opens it at the lowest native rate that is an integer
 * multiple of the request (32 kHz, then 48 kHz) and decimates each block
 * back down. Drives the real backend against src/fake_dmic.c, which refuses
 * every rate below 32 kHz.
 */

#include <zephyr/ztest.h>

#include <alp/audio.h>
#include <alp/peripheral.h>

extern uint32_t fake_dmic_rate;
extern size_t   fake_dmic_block;

#define FPB 64u

static alp_audio_in_t *open_at(uint32_t rate)
{
	alp_audio_config_t cfg = ALP_AUDIO_CONFIG_DEFAULT(0);

	cfg.sample_rate_hz   = rate;
	cfg.channels         = 1;
	cfg.frames_per_block = FPB;
	return alp_audio_in_open(&cfg);
}

ZTEST_SUITE(alp_audio_in_resample, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_audio_in_resample, test_refused_rate_opens_at_integer_multiple)
{
	alp_audio_in_t *in = open_at(16000);

	zassert_not_null(in, "16 kHz must open by resampling");
	zassert_equal(fake_dmic_rate, 32000, "native rate must be 32 kHz (ratio 2)");
	zassert_equal(fake_dmic_block, FPB * 2u * sizeof(int16_t), "native block = ratio x request");

	int16_t buf[FPB];
	size_t  got = 0;

	zassert_ok(alp_audio_in_start(in));
	/* Sample values are not checked here: the backend's DC blocker runs
	 * after decimation and removes the fake's constant level on purpose.
	 * The decimator's own response is covered by tests/unit/dsp_decimator. */
	for (int i = 0; i < 4; i++) {
		zassert_equal(alp_audio_in_read(in, buf, FPB, &got, 100), ALP_OK);
		zassert_equal(got, FPB, "one native block -> frames_per_block frames");
	}
	alp_audio_in_close(in);
}

ZTEST(alp_audio_in_resample, test_small_read_truncates_like_native)
{
	alp_audio_in_t *in = open_at(8000);

	zassert_not_null(in);
	zassert_equal(fake_dmic_rate, 32000, "8 kHz rides 32 kHz (ratio 4)");

	int16_t buf[FPB];
	size_t  got = 0;

	zassert_ok(alp_audio_in_start(in));
	zassert_equal(alp_audio_in_read(in, buf, FPB / 2u, &got, 100),
	              ALP_OK,
	              "a buffer smaller than one block must truncate, not fail");
	zassert_equal(got, FPB / 2u);
	alp_audio_in_close(in);
}

ZTEST(alp_audio_in_resample, test_native_rate_is_untouched)
{
	alp_audio_in_t *in = open_at(48000);

	zassert_not_null(in);
	zassert_equal(fake_dmic_rate, 48000, "an accepted rate must not be resampled");
	zassert_equal(fake_dmic_block, FPB * sizeof(int16_t));
	alp_audio_in_close(in);
}

ZTEST(alp_audio_in_resample, test_no_integer_multiple_still_fails)
{
	/* 44100 divides neither 32000 nor 48000. */
	alp_audio_in_t *in = open_at(11025);

	zassert_is_null(in);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}
