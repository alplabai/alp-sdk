/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * DSP length-range guard (#734).  Split into its own tiny, dependency-free
 * header so the CMSIS block-size boundary is unit-testable on a wide-size_t
 * host without pulling in the DSP backend or a mocked CMSIS layer.
 */
#ifndef ALP_DSP_RANGE_H
#define ALP_DSP_RANGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/alp_checked_arith.h"

/*
 * Narrow a size_t element count to the uint32_t block size the CMSIS-DSP
 * f32 kernels (arm_mean_f32 / arm_rms_f32 / ...) accept.
 *
 * Returns false when @p n cannot be represented as a uint32_t, so the caller
 * returns ALP_ERR_OUT_OF_RANGE BEFORE invoking a kernel that would otherwise
 * silently truncate the count and process only a prefix while the public API
 * reported success (#734).  On success, *@p block_out holds the narrowed
 * length to reuse across every kernel call.
 *
 * On the 32-bit Cortex-M targets that actually compile the CMSIS path,
 * size_t == uint32_t so SIZE_MAX == UINT32_MAX and the bound folds away
 * (no truncation is possible there); the check only bites a host whose
 * size_t is wider (native_sim / desktop, or a mocked-CMSIS test).
 *
 * Delegates to the shared alp_size_to_u32() helper
 * (src/common/alp_checked_arith.h, #743) -- this wrapper keeps its own
 * name/doc so call sites still read "narrow to a CMSIS block size", but no
 * longer carries a second copy of the SIZE_MAX-guarded narrowing logic.
 */
static inline bool alp_dsp_cmsis_block_len(size_t n, uint32_t *block_out)
{
	return alp_size_to_u32(n, block_out);
}

#endif /* ALP_DSP_RANGE_H */
