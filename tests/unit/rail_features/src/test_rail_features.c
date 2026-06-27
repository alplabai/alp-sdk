/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for rail_features (DSP feature extraction) -- native_sim.
 */
#include <math.h>
#include <string.h>
#include <zephyr/ztest.h>
#include "rail_features.h"

ZTEST_SUITE(rail_features, NULL, NULL, NULL, NULL, NULL);

/* Fill a window by pushing N samples produced by gen(i). */
static void fill(struct rail_feat_state *st, float (*gen)(int))
{
	rail_feat_state_reset(st);
	for (int i = 0; i < RAIL_WINDOW_N; i++) {
		rail_feat_window_push(st, gen(i));
	}
}

static float gen_quiet(int i)
{
	(void)i;
	return 0.001f;
}

/* A pure tone at 100 Hz given ODR 800 -> 8 samples/cycle. */
static float gen_tone_100hz(int i)
{
	return sinf(2.0f * (float)M_PI * 100.0f * (float)i / RAIL_ODR_HZ);
}

ZTEST(rail_features, test_window_fill_and_pack_dim)
{
	struct rail_feat_state st;
	struct rail_features   f;
	float                  vec[RAIL_FEATURE_DIM];

	rail_feat_state_reset(&st);
	zassert_false(rail_feat_window_full(&st), "empty window not full");
	fill(&st, gen_quiet);
	zassert_true(rail_feat_window_full(&st), "full window reports full");

	rail_feat_extract(&st, RAIL_ODR_HZ, 0.0f, &f);
	size_t n = rail_feat_pack(&f, vec, RAIL_FEATURE_DIM);
	zassert_equal(n, (size_t)RAIL_FEATURE_DIM, "pack writes RAIL_FEATURE_DIM values");
}

ZTEST(rail_features, test_quiet_is_low_energy)
{
	struct rail_feat_state st;
	struct rail_features   f;

	fill(&st, gen_quiet);
	rail_feat_extract(&st, RAIL_ODR_HZ, 20.0f, &f);
	zassert_true(f.rms < 0.01f, "quiet window has near-zero RMS");
}

ZTEST(rail_features, test_tone_dominant_frequency_and_wavelength)
{
	struct rail_feat_state st;
	struct rail_features   f;

	fill(&st, gen_tone_100hz);
	/* speed 20 m/s, tone 100 Hz -> wavelength 0.20 m. */
	rail_feat_extract(&st, RAIL_ODR_HZ, 20.0f, &f);

	/* FFT bin resolution = 800/256 = 3.125 Hz; 100 Hz lands at bin 32. */
	zassert_within(f.dom_freq_hz, 100.0f, 4.0f, "dominant frequency ~100 Hz");
	zassert_within(f.rail_wavelength_m, 0.20f, 0.02f, "wavelength = speed/freq");
}

ZTEST(rail_features, test_wavelength_guarded_on_zero_speed)
{
	struct rail_feat_state st;
	struct rail_features   f;

	fill(&st, gen_tone_100hz);
	rail_feat_extract(&st, RAIL_ODR_HZ, 0.0f, &f);
	zassert_equal(f.rail_wavelength_m, 0.0f, "zero speed -> wavelength 0 (guarded)");
}

/* Periodic unit impulses every 64 samples -> high crest + high kurtosis. */
static float gen_impulse_train(int i)
{
	return (i % 64 == 0) ? 1.0f : 0.0f;
}

/* Broadband: a cheap deterministic pseudo-noise (no Math.random). */
static float gen_broadband(int i)
{
	float s = sinf((float)i * 1.7f) + sinf((float)i * 0.37f) + sinf((float)i * 3.91f);
	return s * 0.5f;
}

ZTEST(rail_features, test_classify_quiet_is_healthy)
{
	struct rail_feat_state st;
	struct rail_features   f;
	fill(&st, gen_quiet);
	rail_feat_extract(&st, RAIL_ODR_HZ, 20.0f, &f);
	struct rail_verdict v = rail_classify_fallback(&f);
	zassert_equal(v.cls, RAIL_HEALTHY, "quiet -> HEALTHY");
	zassert_true(v.severity < 0.2f, "healthy severity is low");
}

ZTEST(rail_features, test_classify_impulse_is_joint_weld)
{
	struct rail_feat_state st;
	struct rail_features   f;
	fill(&st, gen_impulse_train);
	rail_feat_extract(&st, RAIL_ODR_HZ, 20.0f, &f);
	zassert_true(f.crest_factor > 6.0f, "impulse train has high crest");
	struct rail_verdict v = rail_classify_fallback(&f);
	zassert_equal(v.cls, RAIL_JOINT_WELD, "impulsive -> JOINT_WELD");
}

ZTEST(rail_features, test_classify_tone_is_corrugation)
{
	struct rail_feat_state st;
	struct rail_features   f;
	fill(&st, gen_tone_100hz);
	rail_feat_extract(&st, RAIL_ODR_HZ, 20.0f, &f);
	struct rail_verdict v = rail_classify_fallback(&f);
	zassert_equal(v.cls, RAIL_CORRUGATION, "narrowband tone -> CORRUGATION");
}

ZTEST(rail_features, test_class_name_round_trip)
{
	zassert_true(strcmp(rail_class_name(RAIL_HEALTHY), "HEALTHY") == 0, "name");
	zassert_true(strcmp(rail_class_name(RAIL_JOINT_WELD), "JOINT_WELD") == 0, "name");
}

ZTEST(rail_features, test_classify_broadband_is_rough_rcf)
{
	struct rail_feat_state st;
	struct rail_features   f;
	fill(&st, gen_broadband);
	rail_feat_extract(&st, RAIL_ODR_HZ, 20.0f, &f);
	/* Broadband energy: elevated RMS, no single dominant band. */
	zassert_true(f.rms > 0.30f, "broadband signal has elevated RMS");
	struct rail_verdict v = rail_classify_fallback(&f);
	zassert_equal(v.cls, RAIL_ROUGH_RCF, "broadband -> ROUGH_RCF");
}
