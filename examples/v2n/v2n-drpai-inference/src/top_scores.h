/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * top_scores -- pure-C top-N selection over a flat float array.
 *
 * Pulled out of main.c so the one non-trivial algorithm in this example
 * (finding the N largest raw NPU output values, largest first) has a
 * host-buildable home a real test can include, the same way
 * examples/ai/visual-defect-detection/src/defect_map.{c,h} does for its
 * example.  See tests/unit/top_scores/.
 */
#ifndef TOP_SCORES_H
#define TOP_SCORES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Find the @p max_n largest values in @p values[0..count), largest first.
 *  Ties keep the earliest (lowest-index) occurrence ahead, matching a
 *  strict `>` comparison during the scan.
 *
 *  @param[in]  values  Flat array to scan.
 *  @param[in]  count   Number of elements in @p values.
 *  @param[in]  max_n   Capacity of @p out_idx / @p out_val (the N in top-N).
 *  @param[out] out_idx Flat index of each selected value, largest first.
 *                       Must hold @p max_n elements.
 *  @param[out] out_val The selected values themselves, largest first.
 *                       Must hold @p max_n elements.
 *  @param[out] out_n   Number of entries written (`min(max_n, count)`).
 *
 *  Small-N selection in O(count * max_n) -- fine for a demo-sized tensor
 *  and a single-digit N, not worth a heap here.
 */
void top_scores_select(const float *values,
                       size_t       count,
                       size_t       max_n,
                       size_t      *out_idx,
                       float       *out_val,
                       size_t      *out_n);

#ifdef __cplusplus
}
#endif

#endif /* TOP_SCORES_H */
