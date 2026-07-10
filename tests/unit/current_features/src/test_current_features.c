/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for current_features (INA236 DSP) -- native_sim.
 */
#include <math.h>
#include <string.h>
#include <zephyr/ztest.h>
#include "current_features.h"

ZTEST_SUITE(current_features, NULL, NULL, NULL, NULL, NULL);

/* Fill a window with constant current + a 40 Hz ripple of the given amplitude. */
static void fill_current(struct curr_window_state *st, float dc_a, float ripple_a)
{
	curr_window_reset(st);
	for (int i = 0; i < CURR_WINDOW_N; i++) {
		float              t = (float)i / CURR_SR_HZ;
		struct curr_sample s = {
			.current_a = dc_a + ripple_a * sinf(2.0f * (float)M_PI * 40.0f * t),
			.bus_v     = 12.0f,
			.power_w   = 12.0f * dc_a,
		};
		curr_window_push(st, s);
	}
}

ZTEST(current_features, test_fill_and_pack_dim)
{
	struct curr_window_state st;
	struct curr_features     f;
	float                    vec[CURR_FEATURE_DIM];

	curr_window_reset(&st);
	zassert_false(curr_window_full(&st), "empty window not full");
	fill_current(&st, 1.0f, 0.1f);
	zassert_true(curr_window_full(&st), "full window reports full");

	curr_feat_extract(&st, CURR_SR_HZ, &f);
	zassert_equal(curr_feat_pack(&f, vec, CURR_FEATURE_DIM),
	              (size_t)CURR_FEATURE_DIM,
	              "pack writes CURR_FEATURE_DIM");
}

ZTEST(current_features, test_means_and_ripple)
{
	struct curr_window_state st;
	struct curr_features     f;

	fill_current(&st, 1.0f, 0.1f); /* 1 A DC + 0.1 A ripple at 40 Hz */
	curr_feat_extract(&st, CURR_SR_HZ, &f);

	zassert_within((double)f.mean_current_a, 1.0, 0.02, "mean current ~1 A");
	zassert_within((double)f.rms_ac_a, 0.0707, 0.02, "ripple RMS ~ 0.1/sqrt(2)");
	zassert_within((double)f.ripple_freq_hz, 40.0, 3.0, "ripple at ~40 Hz");
	zassert_within((double)f.mean_bus_v, 12.0, 0.1, "mean bus voltage");
}

ZTEST(current_features, test_inrush_slope_is_negative)
{
	struct curr_window_state st;
	struct curr_features     f;

	/* Startup inrush: 5 A decaying linearly to 1 A across the window. */
	curr_window_reset(&st);
	for (int i = 0; i < CURR_WINDOW_N; i++) {
		float              frac = (float)i / (float)(CURR_WINDOW_N - 1);
		struct curr_sample s    = { .current_a = 5.0f - 4.0f * frac,
			                        .bus_v     = 12.0f,
			                        .power_w   = 36.0f };
		curr_window_push(&st, s);
	}
	curr_feat_extract(&st, CURR_SR_HZ, &f);
	zassert_true(f.slope_a < -1.0f, "decaying inrush -> strongly negative slope");
}

static const struct curr_config CFG = { .off_a          = 0.05f,
	                                    .overload_a     = 2.5f,
	                                    .ripple_min_a   = 0.05f,
	                                    .inrush_slope_a = 1.0f };

static struct curr_features feat_of(float dc_a, float ripple_a)
{
	struct curr_window_state st;
	struct curr_features     f;
	fill_current(&st, dc_a, ripple_a);
	curr_feat_extract(&st, CURR_SR_HZ, &f);
	return f;
}

ZTEST(current_features, test_classify_off_and_normal)
{
	struct curr_features off = feat_of(0.01f, 0.0f);
	zassert_equal(current_classify(&off, &CFG), CURR_OFF, "near-zero -> OFF");

	struct curr_features nom = feat_of(1.0f, 0.1f);
	zassert_equal(current_classify(&nom, &CFG), CURR_NORMAL, "1 A + ripple -> NORMAL");
}

ZTEST(current_features, test_classify_overload_vs_stall)
{
	/* Both draw 3 A (> overload), but stall has NO ripple. */
	struct curr_features over  = feat_of(3.0f, 0.1f);
	struct curr_features stall = feat_of(3.0f, 0.0f);
	zassert_equal(current_classify(&over, &CFG), CURR_OVERLOAD, "high A + ripple -> OVERLOAD");
	zassert_equal(current_classify(&stall, &CFG), CURR_STALL, "high A + no ripple -> STALL");
}

ZTEST(current_features, test_classify_inrush)
{
	struct curr_window_state st;
	struct curr_features     f;
	curr_window_reset(&st);
	for (int i = 0; i < CURR_WINDOW_N; i++) {
		float              frac = (float)i / (float)(CURR_WINDOW_N - 1);
		struct curr_sample s    = { .current_a = 5.0f - 4.0f * frac,
			                        .bus_v     = 12.0f,
			                        .power_w   = 36.0f };
		curr_window_push(&st, s);
	}
	curr_feat_extract(&st, CURR_SR_HZ, &f);
	zassert_equal(current_classify(&f, &CFG), CURR_INRUSH, "decaying startup -> INRUSH");
}

ZTEST(current_features, test_anomaly_and_names)
{
	struct curr_features nom   = feat_of(1.0f, 0.1f);
	struct curr_features stall = feat_of(3.0f, 0.0f);
	zassert_true(curr_anomaly_fallback(&nom, &CFG) < 0.2f, "healthy -> low anomaly");
	zassert_true(curr_anomaly_fallback(&stall, &CFG) > 0.8f, "stall -> high anomaly");

	zassert_true(strcmp(curr_state_name(CURR_OFF), "OFF") == 0, "name");
	zassert_true(strcmp(curr_state_name(CURR_STALL), "STALL") == 0, "name");
}
