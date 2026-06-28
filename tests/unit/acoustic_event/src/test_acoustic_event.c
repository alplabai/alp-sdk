/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for acoustic_event (per-frame DSP) -- native_sim.
 */
#include <math.h>
#include <zephyr/ztest.h>
#include "acoustic_event.h"

ZTEST_SUITE(acoustic_event, NULL, NULL, NULL, NULL, NULL);

static void fill_tone(struct ase_frame_state *st, float freq_hz, float amp)
{
	ase_frame_reset(st);
	for (int i = 0; i < ASE_FRAME_N; i++) {
		ase_frame_push(st, amp * sinf(2.0f * (float)M_PI * freq_hz * (float)i / ASE_SR_HZ));
	}
}

ZTEST(acoustic_event, test_fill_and_pack_dim)
{
	struct ase_frame_state st;
	struct ase_features    f;
	float                  vec[ASE_FEATURE_DIM];

	ase_frame_reset(&st);
	zassert_false(ase_frame_full(&st), "empty frame not full");
	fill_tone(&st, 3000.0f, 0.3f);
	zassert_true(ase_frame_full(&st), "full frame reports full");

	ase_feat_extract(&st, ASE_SR_HZ, &f);
	zassert_equal(ase_feat_pack(&f, vec, ASE_FEATURE_DIM),
	              (size_t)ASE_FEATURE_DIM,
	              "pack writes ASE_FEATURE_DIM");
}

ZTEST(acoustic_event, test_tone_centroid_and_flatness)
{
	struct ase_frame_state st;
	struct ase_features    f;

	fill_tone(&st, 3000.0f, 0.3f);
	ase_feat_extract(&st, ASE_SR_HZ, &f);

	zassert_within((double)f.centroid_hz, 3000.0, 250.0, "tone centroid near 3 kHz");
	zassert_true(f.flatness < 0.1f, "a pure tone is very non-flat");
	zassert_within((double)f.rms, 0.2121, 0.02, "0.3 amp tone RMS ~ 0.3/sqrt(2)");
}

ZTEST(acoustic_event, test_zcr_rises_with_frequency)
{
	struct ase_frame_state st;
	struct ase_features    lo, hi;

	fill_tone(&st, 1000.0f, 0.3f);
	ase_feat_extract(&st, ASE_SR_HZ, &lo);
	fill_tone(&st, 6000.0f, 0.3f);
	ase_feat_extract(&st, ASE_SR_HZ, &hi);
	zassert_true(hi.zcr > lo.zcr, "ZCR increases with tone frequency");
	zassert_within((double)hi.zcr, 0.75, 0.1, "6 kHz / 16 kHz -> ZCR ~0.75");
}
