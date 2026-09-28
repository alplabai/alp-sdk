/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for top_scores_select() (the DRP-AI example's top-N
 * raw-score selection) -- native_sim, hand-built float arrays.
 */
#include <math.h>

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

ZTEST(top_scores, test_nan_is_skipped_not_ranked)
{
	/* A NaN would otherwise take rank #1 (it admits unconditionally
	 * while the window isn't full, then can never be displaced -- every
	 * ordered comparison against it is false) and demote the true
	 * maximum. Realistic input: the example's README notes the model
	 * was quantised against non-representative calibration frames. */
	static const float values[] = { NAN, 1.0f, 2.0f, 3.0f, 100.0f, 50.0f };
	size_t             idx[3];
	float              val[3];
	size_t             n = 0;

	top_scores_select(values, 6, 3, idx, val, &n);

	zassert_equal(n, 3, "3 finite values selected, NaN skipped");
	zassert_equal(idx[0], 4, "largest is values[4]=100.0, not the NaN");
	zassert_within((double)val[0], 100.0, 1e-6, "val[0]=100.0");
	zassert_equal(idx[1], 5, "2nd largest is values[5]=50.0");
	zassert_within((double)val[1], 50.0, 1e-6, "val[1]=50.0");
	zassert_equal(idx[2], 3, "3rd largest is values[3]=3.0");
	zassert_within((double)val[2], 3.0, 1e-6, "val[2]=3.0");
}

ZTEST(top_scores, test_infinity_is_skipped_not_ranked)
{
	/* +-Inf are non-finite too and must be skipped exactly like NaN --
	 * in particular +Inf must NOT win rank #1 just because it compares
	 * true against every finite value; skipping it discards what would
	 * otherwise be the largest score (see top_scores.h). */
	static const float values[] = { INFINITY, 1.0f, -INFINITY, 42.0f, 7.0f };
	size_t             idx[3];
	float              val[3];
	size_t             n = 0;

	top_scores_select(values, 5, 3, idx, val, &n);

	zassert_equal(n, 3, "3 finite values selected, +-Inf skipped");
	zassert_equal(idx[0], 3, "largest is values[3]=42.0, not +Inf");
	zassert_within((double)val[0], 42.0, 1e-6, "val[0]=42.0");
	zassert_equal(idx[1], 4, "2nd largest is values[4]=7.0");
	zassert_within((double)val[1], 7.0, 1e-6, "val[1]=7.0");
	zassert_equal(idx[2], 1, "3rd largest is values[1]=1.0");
	zassert_within((double)val[2], 1.0, 1e-6, "val[2]=1.0");
}

ZTEST(top_scores, test_all_nan_yields_zero_selected)
{
	static const float values[] = { NAN, NAN, NAN, NAN, NAN };
	size_t             idx[5];
	float              val[5];
	size_t             n = 123; /* poison -- must be overwritten with 0 */

	top_scores_select(values, 5, 5, idx, val, &n);

	zassert_equal(n, 0, "every input is NaN -> nothing ranked");
}

ZTEST(top_scores, test_max_n_zero_does_not_underflow)
{
	/* max_n == 0 must not evaluate out_val[max_n - 1] (SIZE_MAX,
	 * out-of-bounds): the short-circuit `n < max_n` is false for
	 * n=max_n=0, so the OR's second operand WOULD be evaluated without
	 * the explicit early return. */
	static const float values[] = { 1.0f, 2.0f, 3.0f };
	size_t             n        = 123; /* poison -- must be overwritten with 0 */

	top_scores_select(values, 3, 0, NULL, NULL, &n);

	zassert_equal(n, 0, "max_n=0 -> n=0, no out-of-bounds access");
}
