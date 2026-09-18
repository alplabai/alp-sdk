/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for top_scores_select() (the DRP-AI example's top-N
 * raw-score selection) -- native_sim, hand-built float arrays.
 */
#include <zephyr/ztest.h>
#include "top_scores.h"

ZTEST_SUITE(top_scores, NULL, NULL, NULL, NULL, NULL);

ZTEST(top_scores, test_basic_top3_of_7)
{
	static const float values[] = { 1.0f, 5.0f, 3.0f, 9.0f, 2.0f, 8.0f, 4.0f };
	size_t             idx[3];
	float              val[3];
	size_t             n = 0;

	top_scores_select(values, 7, 3, idx, val, &n);

	zassert_equal(n, 3, "3 of 7 selected");
	zassert_equal(idx[0], 3, "largest is values[3]=9.0");
	zassert_within((double)val[0], 9.0, 1e-6, "val[0]=9.0");
	zassert_equal(idx[1], 5, "2nd largest is values[5]=8.0");
	zassert_within((double)val[1], 8.0, 1e-6, "val[1]=8.0");
	zassert_equal(idx[2], 1, "3rd largest is values[1]=5.0");
	zassert_within((double)val[2], 5.0, 1e-6, "val[2]=5.0");
}

ZTEST(top_scores, test_count_less_than_max_n)
{
	/* Fewer input values than requested slots: out_n caps at count, and
	 * only the filled slots are meaningful. */
	static const float values[] = { 4.0f, 1.0f };
	size_t             idx[5];
	float              val[5];
	size_t             n = 0;

	top_scores_select(values, 2, 5, idx, val, &n);

	zassert_equal(n, 2, "only 2 inputs -> n=2, not max_n=5");
	zassert_equal(idx[0], 0, "largest is values[0]=4.0");
	zassert_equal(idx[1], 1, "2nd is values[1]=1.0");
}

ZTEST(top_scores, test_empty_input)
{
	size_t idx[3];
	float  val[3];
	size_t n = 123; /* poison -- must be overwritten with 0 */

	top_scores_select(NULL, 0, 3, idx, val, &n);

	zassert_equal(n, 0, "no values -> n=0");
}

ZTEST(top_scores, test_ties_keep_earliest_index_ahead)
{
	/* Equal values: the earlier index must sort ahead (strict `>` during
	 * insertion, so a tie never displaces what is already kept). */
	static const float values[] = { 7.0f, 7.0f, 7.0f };
	size_t             idx[2];
	float              val[2];
	size_t             n = 0;

	top_scores_select(values, 3, 2, idx, val, &n);

	zassert_equal(n, 2, "2 of 3 selected");
	zassert_equal(idx[0], 0, "first 7.0 (index 0) sorts ahead on a tie");
	zassert_equal(idx[1], 1, "second 7.0 (index 1) is the runner-up");
}

ZTEST(top_scores, test_negative_and_descending_order)
{
	static const float values[] = { -1.0f, -5.0f, -2.0f, 0.0f };
	size_t             idx[4];
	float              val[4];
	size_t             n = 0;

	top_scores_select(values, 4, 4, idx, val, &n);

	zassert_equal(n, 4, "all 4 selected");
	/* Full output must be sorted largest-first. */
	zassert_within((double)val[0], 0.0, 1e-6, "val[0]=0.0");
	zassert_within((double)val[1], -1.0, 1e-6, "val[1]=-1.0");
	zassert_within((double)val[2], -2.0, 1e-6, "val[2]=-2.0");
	zassert_within((double)val[3], -5.0, 1e-6, "val[3]=-5.0");
}
