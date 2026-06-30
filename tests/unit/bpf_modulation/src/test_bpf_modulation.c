/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for bpf_modulation (blade-order envelope analysis) -- native_sim.
 */
#include <math.h>
#include <zephyr/ztest.h>
#include "bpf_modulation.h"

ZTEST_SUITE(bpf_modulation, NULL, NULL, NULL, NULL, NULL);

static void fill_am(struct bpf_env_state *st, float bpf, float fr, float depth)
{
	bpf_env_reset(st);
	for (int i = 0; i < BPF_ENV_N; i++) {
		bpf_env_push(st, 1.0f + depth * sinf(2.0f * (float)M_PI * bpf * (float)i / fr));
	}
}

ZTEST(bpf_modulation, test_blade_order_peaks_at_bpf)
{
	struct bpf_env_state  st;
	struct bpf_modulation m;
	const float           fr = 62.5f, bpf = 0.75f;

	fill_am(&st, bpf, fr, 0.5f);
	bpf_modulation_extract(&st, bpf, fr, &m);

	/* Fundamental blade order carries far more energy than the 2nd harmonic. */
	zassert_true(m.blade_order_energy[0] > 5.0f * m.blade_order_energy[1],
	             "fundamental dominates the 2nd harmonic for a pure AM tone");
	/* (max-min)/(max+min) of 1 +/- 0.5 = 0.5. */
	zassert_within((double)m.modulation_depth, 0.5, 0.05, "modulation depth ~0.5");
}

ZTEST(bpf_modulation, test_flat_envelope_has_no_order_energy)
{
	struct bpf_env_state  st;
	struct bpf_modulation m;

	bpf_env_reset(&st);
	for (int i = 0; i < BPF_ENV_N; i++) {
		bpf_env_push(&st, 1.0f); /* flat */
	}
	bpf_modulation_extract(&st, 0.75f, 62.5f, &m);

	zassert_true(m.blade_order_energy[0] < 1e-3f, "flat envelope -> ~0 order energy");
	zassert_within((double)m.modulation_depth, 0.0, 1e-3, "flat -> zero modulation depth");
}

ZTEST(bpf_modulation, test_pack_dim)
{
	struct bpf_modulation m = { 0 };
	float                 vec[BPF_FEATURE_DIM];
	zassert_equal(bpf_modulation_pack(&m, vec, BPF_FEATURE_DIM),
	              (size_t)BPF_FEATURE_DIM,
	              "pack writes BPF_FEATURE_DIM");
}
