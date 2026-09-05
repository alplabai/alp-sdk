/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * top_scores implementation -- see top_scores.h.
 *
 * Single pass over the input: keep a sorted (largest-first) window of the
 * best max_n values seen so far, insertion-sorting each new candidate into
 * the window when it beats the current worst kept value.
 */
#include "top_scores.h"

void top_scores_select(const float *values,
                       size_t       count,
                       size_t       max_n,
                       size_t      *out_idx,
                       float       *out_val,
                       size_t      *out_n)
{
	size_t n = 0;

	for (size_t i = 0; i < count; ++i) {
		float v = values[i];

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
