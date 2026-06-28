/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for acoustic_features (per-frame DSP) -- native_sim.
 */
#include <math.h>
#include <zephyr/ztest.h>
#include "acoustic_features.h"

ZTEST_SUITE(acoustic_features, NULL, NULL, NULL, NULL, NULL);

static void fill(struct aco_frame_state *st, float (*gen)(int))
{
	aco_frame_reset(st);
	for (int i = 0; i < ACO_FRAME_N; i++) {
		aco_frame_push(st, gen(i));
	}
}

/* Low-amplitude broadband: three incommensurate tones -> flat-ish spectrum. */
static float gen_lownoise(int i)
{
	return 0.02f * (sinf((float)i * 1.7f) + sinf((float)i * 0.37f) + sinf((float)i * 3.91f));
}

/* A 1 kHz tone at 16 kHz ODR -> single spectral peak. */
static float gen_tone_1k(int i)
{
	return sinf(2.0f * (float)M_PI * 1000.0f * (float)i / ACO_SR_HZ);
}

/* Periodic impulses -> high kurtosis. */
static float gen_impulse(int i)
{
	return (i % 64 == 0) ? 1.0f : 0.0f;
}

ZTEST(acoustic_features, test_fill_and_pack_dim)
{
	struct aco_frame_state st;
	struct aco_features    f;
	float                  vec[ACO_FEATURE_DIM];

	aco_frame_reset(&st);
	zassert_false(aco_frame_full(&st), "empty frame not full");
	fill(&st, gen_lownoise);
	zassert_true(aco_frame_full(&st), "full frame reports full");

	aco_feat_extract(&st, ACO_SR_HZ, &f);
	zassert_equal(aco_feat_pack(&f, vec, ACO_FEATURE_DIM),
	              (size_t)ACO_FEATURE_DIM,
	              "pack writes ACO_FEATURE_DIM");
}

ZTEST(acoustic_features, test_tone_is_less_flat_than_noise)
{
	struct aco_frame_state st;
	struct aco_features    fn, ft;

	fill(&st, gen_lownoise);
	aco_feat_extract(&st, ACO_SR_HZ, &fn);
	fill(&st, gen_tone_1k);
	aco_feat_extract(&st, ACO_SR_HZ, &ft);

	zassert_true(ft.spectral_flatness < fn.spectral_flatness,
	             "a pure tone is spectrally less flat than broadband");
	zassert_within((double)ft.spectral_centroid_hz, 1000.0, 150.0, "tone centroid near 1 kHz");
}

ZTEST(acoustic_features, test_impulse_has_high_kurtosis)
{
	struct aco_frame_state st;
	struct aco_features    f;

	fill(&st, gen_impulse);
	aco_feat_extract(&st, ACO_SR_HZ, &f);
	zassert_true(f.kurtosis > 5.0f, "impulse train has high kurtosis");
}

ZTEST(acoustic_features, test_anomaly_zero_on_baseline_high_off_baseline)
{
	struct aco_baseline base;
	for (int i = 0; i < ACO_FEATURE_DIM; i++) {
		base.mean[i]    = 1.0f;
		base.inv_var[i] = 1.0f;
	}

	float on[ACO_FEATURE_DIM];
	float off[ACO_FEATURE_DIM];
	for (int i = 0; i < ACO_FEATURE_DIM; i++) {
		on[i]  = 1.0f;        /* exactly the baseline mean */
		off[i] = 1.0f + 3.0f; /* 3 sigma off on every feature */
	}

	float s_on  = aco_anomaly_fallback(on, ACO_FEATURE_DIM, &base);
	float s_off = aco_anomaly_fallback(off, ACO_FEATURE_DIM, &base);

	zassert_within((double)s_on, 0.0, 1e-4, "score ~0 at the baseline mean");
	zassert_true(s_off > 0.9f, "score saturates high far off baseline");
	zassert_true(s_off <= 1.0f, "score is clamped to <= 1");
}
