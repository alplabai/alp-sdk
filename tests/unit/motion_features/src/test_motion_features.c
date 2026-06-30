/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for motion_features (windowed IMU DSP) -- native_sim.
 */
#include <math.h>
#include <string.h>
#include <zephyr/ztest.h>
#include "motion_features.h"

ZTEST_SUITE(motion_features, NULL, NULL, NULL, NULL, NULL);

/* Fill a window: gravity on Z plus a vertical bounce at freq_hz with the given
 * accel amplitude (g); gyro left near zero. */
static void fill_gait(struct mot_window_state *st, float freq_hz, float amp_g)
{
	mot_window_reset(st);
	for (int i = 0; i < MOT_WINDOW_N; i++) {
		float             t = (float)i / MOT_SR_HZ;
		struct mot_sample s = {
			.ax = 0.03f * sinf(2.0f * (float)M_PI * freq_hz * t),
			.ay = 0.03f * sinf(2.0f * (float)M_PI * freq_hz * t + 1.0f),
			.az = 1.0f + amp_g * sinf(2.0f * (float)M_PI * freq_hz * t),
			.gx = 0.0f,
			.gy = 0.0f,
			.gz = 0.0f,
		};
		mot_window_push(st, s);
	}
}

ZTEST(motion_features, test_fill_and_pack_dim)
{
	struct mot_window_state st;
	struct mot_features     f;
	float                   vec[MOT_FEATURE_DIM];

	mot_window_reset(&st);
	zassert_false(mot_window_full(&st), "empty window not full");
	fill_gait(&st, 2.0f, 0.3f);
	zassert_true(mot_window_full(&st), "full window reports full");

	mot_feat_extract(&st, MOT_SR_HZ, &f);
	zassert_equal(mot_feat_pack(&f, vec, MOT_FEATURE_DIM),
	              (size_t)MOT_FEATURE_DIM,
	              "pack writes MOT_FEATURE_DIM");
}

ZTEST(motion_features, test_walk_dominant_frequency)
{
	struct mot_window_state st;
	struct mot_features     f;

	fill_gait(&st, 2.0f, 0.3f); /* 2 Hz step cadence */
	mot_feat_extract(&st, MOT_SR_HZ, &f);
	zassert_within((double)f.dom_freq_hz, 2.0, 0.6, "walk cadence ~2 Hz");
	zassert_true(f.amag_rms > 0.05f, "moving window has nonzero AC magnitude");
}

ZTEST(motion_features, test_idle_is_low_energy)
{
	struct mot_window_state st;
	struct mot_features     f;

	mot_window_reset(&st);
	for (int i = 0; i < MOT_WINDOW_N; i++) {
		struct mot_sample s = {
			.ax = 0.0f, .ay = 0.0f, .az = 1.0f, .gx = 0.0f, .gy = 0.0f, .gz = 0.0f
		};
		mot_window_push(&st, s);
	}
	mot_feat_extract(&st, MOT_SR_HZ, &f);
	zassert_true(f.amag_rms < 0.02f, "idle window has near-zero AC magnitude");
	zassert_within((double)f.tilt_deg, 0.0, 5.0, "Z-up gravity -> ~0 deg tilt");
}

ZTEST(motion_features, test_classify_idle)
{
	struct mot_window_state st;
	struct mot_features     f;
	mot_window_reset(&st);
	for (int i = 0; i < MOT_WINDOW_N; i++) {
		struct mot_sample s = { .ax = 0.0f, .ay = 0.0f, .az = 1.0f };
		mot_window_push(&st, s);
	}
	mot_feat_extract(&st, MOT_SR_HZ, &f);
	zassert_equal(mot_activity_fallback(&f).cls, ACT_IDLE, "still -> IDLE");
}

ZTEST(motion_features, test_classify_walk_vs_run)
{
	struct mot_window_state st;
	struct mot_features     f;

	fill_gait(&st, 2.0f, 0.3f); /* 2 Hz, modest amplitude -> WALK */
	mot_feat_extract(&st, MOT_SR_HZ, &f);
	zassert_equal(mot_activity_fallback(&f).cls, ACT_WALK, "2 Hz modest -> WALK");

	fill_gait(&st, 3.0f, 1.2f); /* 3 Hz, large amplitude -> RUN */
	mot_feat_extract(&st, MOT_SR_HZ, &f);
	zassert_equal(mot_activity_fallback(&f).cls, ACT_RUN, "3 Hz strong -> RUN");
}

ZTEST(motion_features, test_activity_name)
{
	zassert_true(strcmp(mot_activity_name(ACT_IDLE), "IDLE") == 0, "name");
	zassert_true(strcmp(mot_activity_name(ACT_RUN), "RUN") == 0, "name");
	zassert_true(strcmp(mot_activity_name(ACT_STAIRS), "STAIRS") == 0, "name");
}
