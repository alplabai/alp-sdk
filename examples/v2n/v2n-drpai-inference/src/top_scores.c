/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * top_scores implementation -- see top_scores.h.
 *
 * Single pass over the input: keep a sorted (largest-first) window of the
 * best max_n values seen so far, insertion-sorting each new candidate into
 * the window when it beats the current worst kept value.
 *
 * Non-finite inputs (NaN, +-Inf) are skipped, not ranked: a NaN compares
 * false against every ordered comparison, including its own, so it would
 * otherwise pass the "window not full yet" admission unconditionally and
 * then never be displaceable by a real value -- realistic here, since a
 * quantised NPU model can emit NaN on a garbage frame (see top_scores.h).
 */
#include "top_scores.h"

#include <math.h>

void top_scores_select(const float *values,
                       size_t       count,
                       size_t       max_n,
                       size_t      *out_idx,
                       float       *out_val,
                       size_t      *out_n)
{
	size_t n = 0;

	if (max_n == 0u) {
		*out_n = 0;
		return;
	}

	for (size_t i = 0; i < count; ++i) {
		float v = values[i];

		if (!isfinite(v)) {
			continue;
		}

		if (n < max_n || v > out_val[max_n - 1]) {
			size_t pos = (n < max_n) ? n++ : max_n - 1;
			while (pos > 0 && out_val[pos - 1] < v) {
				out_val[pos] = out_val[pos - 1];
				out_idx[pos] = out_idx[pos - 1];
				--pos;
			}
			out_val[pos] = v;
			out_idx[pos] = i;
		}
	}

	*out_n = n;
}
