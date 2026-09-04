/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #1648 tier 1: `alp_audio_out_set_volume()` recorded a non-unity
 * volume for S24_LE / S32_LE handles and returned ALP_OK, but neither
 * the Yocto (ALSA) nor the Zephyr (I2S) audio-out backend can
 * actually apply a software scale to those formats (both only scale
 * S16_LE) -- so a handle opened S24/S32 played at full scale
 * regardless of what the caller asked for, with no error anywhere.
 * Exercises the shared guard, alp_audio_volume_settable(), that now
 * refuses that combination with ALP_ERR_NOSUPPORT at the
 * out_set_volume entry point -- on the host, no I2S / ALSA device
 * involved.
 */
#include <zephyr/ztest.h>

#include "audio_volume_guard.h"

ZTEST_SUITE(audio_volume_guard, NULL, NULL, NULL, NULL, NULL);

/* Full-scale (255) is a scale-identity no-op on every format, so it
 * must always be accepted -- refusing it would break callers who
 * harmlessly set 255 on a format this SDK can't yet scale.
 */
ZTEST(audio_volume_guard, test_unity_accepted_on_every_format)
{
	zassert_true(alp_audio_volume_settable(ALP_AUDIO_FMT_S16_LE, 255u));
	zassert_true(alp_audio_volume_settable(ALP_AUDIO_FMT_S24_LE, 255u));
	zassert_true(alp_audio_volume_settable(ALP_AUDIO_FMT_S32_LE, 255u));
}

/* S16_LE is the one format both backends can actually scale -- any
 * volume must be accepted.
 */
ZTEST(audio_volume_guard, test_any_volume_accepted_on_s16le)
{
	zassert_true(alp_audio_volume_settable(ALP_AUDIO_FMT_S16_LE, 0u));
	zassert_true(alp_audio_volume_settable(ALP_AUDIO_FMT_S16_LE, 1u));
	zassert_true(alp_audio_volume_settable(ALP_AUDIO_FMT_S16_LE, 128u));
	zassert_true(alp_audio_volume_settable(ALP_AUDIO_FMT_S16_LE, 254u));
}

/* A non-unity volume on S24_LE / S32_LE is exactly issue #1648's
 * silent-drop case -- must be refused.
 */
ZTEST(audio_volume_guard, test_non_unity_refused_on_s24_and_s32)
{
	zassert_false(alp_audio_volume_settable(ALP_AUDIO_FMT_S24_LE, 0u));
	zassert_false(alp_audio_volume_settable(ALP_AUDIO_FMT_S24_LE, 128u));
	zassert_false(alp_audio_volume_settable(ALP_AUDIO_FMT_S24_LE, 254u));
	zassert_false(alp_audio_volume_settable(ALP_AUDIO_FMT_S32_LE, 0u));
	zassert_false(alp_audio_volume_settable(ALP_AUDIO_FMT_S32_LE, 128u));
	zassert_false(alp_audio_volume_settable(ALP_AUDIO_FMT_S32_LE, 254u));
}
