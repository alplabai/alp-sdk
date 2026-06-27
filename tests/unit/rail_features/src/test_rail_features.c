/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for rail_features (DSP feature extraction) -- native_sim.
 */
#include <math.h>
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
