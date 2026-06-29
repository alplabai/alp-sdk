/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for cold_chain (BME280 time-series metrics) -- native_sim.
 */
#include <math.h>
#include <zephyr/ztest.h>
#include "cold_chain.h"

ZTEST_SUITE(cold_chain, NULL, NULL, NULL, NULL, NULL);

/* A vaccine-fridge config: safe band 2..8 C. */
static const struct cc_config CFG = { .t_lo                = 2.0f,
	                                  .t_hi                = 8.0f,
	                                  .mkt_limit_c         = 8.0f,
	                                  .excursion_min_limit = 30.0f,
	                                  .dewpoint_margin_c   = 2.0f };

/* Fill a window with constant temperature + humidity. */
static void fill_const(struct cc_window_state *st, float temp_c, float rh_pct)
{
	cc_window_reset(st);
	for (int i = 0; i < CC_WINDOW_N; i++) {
		struct cc_sample s = { .temp_c = temp_c, .rh_pct = rh_pct, .pressure_pa = 101325.0f };
		cc_window_push(st, s);
	}
}

ZTEST(cold_chain, test_fill_and_pack_dim)
{
	struct cc_window_state st;
	struct cc_features     f;
	float                  vec[CC_FEATURE_DIM];

	cc_window_reset(&st);
	zassert_false(cc_window_full(&st), "empty window not full");
	fill_const(&st, 5.0f, 50.0f);
	zassert_true(cc_window_full(&st), "full window reports full");

	cc_feat_extract(&st, &CFG, &f);
	zassert_equal(cc_feat_pack(&f, vec, CC_FEATURE_DIM),
	              (size_t)CC_FEATURE_DIM,
	              "pack writes CC_FEATURE_DIM");
}

ZTEST(cold_chain, test_mkt_of_constant_equals_temperature)
{
	struct cc_window_state st;
	struct cc_features     f;

	fill_const(&st, 5.0f, 50.0f);
	cc_feat_extract(&st, &CFG, &f);
	/* For a constant profile, MKT == that temperature. */
	zassert_within((double)f.mkt_c, 5.0, 0.05, "MKT of constant 5 C is 5 C");
	zassert_within((double)f.excursion_min, 0.0, 0.01, "5 C is inside 2..8 -> no excursion");
}

ZTEST(cold_chain, test_mkt_exceeds_arithmetic_mean_on_spike)
{
	struct cc_window_state st;
	struct cc_features     f;

	/* 250 samples at 4 C + 6 brief samples at 40 C: arithmetic mean ~4.8 C
	 * (still in-band), but the hot spike pulls MKT well above 8 C. */
	cc_window_reset(&st);
	for (int i = 0; i < 250; i++) {
		struct cc_sample s = { .temp_c = 4.0f, .rh_pct = 50.0f, .pressure_pa = 101325.0f };
		cc_window_push(&st, s);
	}
	for (int i = 0; i < 6; i++) {
		struct cc_sample s = { .temp_c = 40.0f, .rh_pct = 50.0f, .pressure_pa = 101325.0f };
		cc_window_push(&st, s);
	}
	cc_feat_extract(&st, &CFG, &f);
	zassert_true(f.mean_temp_c < 8.0f, "arithmetic mean stays in-band (~4.8 C)");
	zassert_true(f.mkt_c > 8.0f, "MKT exceeds the limit because the spike weighs heavily");
	zassert_true(f.mkt_c > f.mean_temp_c, "MKT >= arithmetic mean (Jensen)");
}

ZTEST(cold_chain, test_dewpoint_magnus)
{
	struct cc_window_state st;
	struct cc_features     f;

	fill_const(&st, 20.0f, 50.0f);
	cc_feat_extract(&st, &CFG, &f);
	/* Magnus dewpoint for 20 C / 50% RH is ~9.3 C. */
	zassert_within((double)f.dewpoint_c, 9.3, 0.5, "dewpoint at 20 C / 50% RH");
}

ZTEST(cold_chain, test_excursion_minutes)
{
	struct cc_window_state st;
	struct cc_features     f;

	fill_const(&st, 12.0f, 55.0f); /* all out of the 2..8 band */
	cc_feat_extract(&st, &CFG, &f);
	zassert_within((double)f.excursion_min,
	               (double)CC_WINDOW_N * (double)CC_SAMPLE_MIN,
	               0.5,
	               "every sample out of band -> full window of excursion minutes");
}
