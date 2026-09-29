/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared rank/shape-copy helper for the ORT (inference_ort.cpp) and DEEPX
 * (inference_deepx.cpp) Yocto inference backends (issue #1729).
 *
 * `alp_inference_tensor_t` ([ABI-STABLE] include/alp/inference.h) carries a
 * fixed `shape[4]`.  Both backends used to silently truncate a rank > 4
 * tensor to its first 4 dims -- a plausible-looking but WRONG shape for any
 * caller computing dimensions or strides from `shape[]` (a caller that only
 * reads `size_bytes` was unaffected either way; that field was always sized
 * correctly).  Per ADR-0002's error-code convention, a capability gap like
 * this -- a real tensor shape the fixed descriptor cannot represent -- is
 * signalled as ALP_ERR_NOSUPPORT, not a best-effort guess; see the
 * "Copy or refuse" rationale on fill_fixed_shape() below.
 *
 * Split into its own header, standalone from either backend, for the same
 * reason as src/yocto/drpai_deploy_shapes.h (#1635): this decision has ZERO
 * dependency on the ONNX Runtime C API or dx_rt, so
 * tests/yocto/inference_tensor_shape.cpp can exercise it directly with a
 * plain host compiler -- no vendor SDK, no NPU hardware.  Every function
 * below is `inline` (header-only, safe to include from both backend TUs
 * without an ODR violation) and takes/returns only <alp/inference.h> and
 * standard types, so it pulls in nothing vendor-specific either.
 */
#ifndef ALP_SDK_YOCTO_INFERENCE_TENSOR_SHAPE_H
#define ALP_SDK_YOCTO_INFERENCE_TENSOR_SHAPE_H

#include <cstddef>
#include <cstdint>

#include "alp/inference.h"

namespace alp_inference_shape
{

/** Fill @p out's `rank`/`shape[4]` from @p dims (an array of @p rank
 *  int64_t dims, most-significant first) -- COPY when it fits, REFUSE when
 *  it doesn't; never truncate.
 *
 *  @return true, with @p out->rank = @p rank and @p out->shape[0..rank)
 *          copied (entries `rank..3` zeroed), when @p rank <= 4 -- the
 *          normal case.
 *  @return false, leaving @p out entirely untouched, when @p rank > 4 --
 *          the tensor's real rank does not fit the fixed-width descriptor.
 *          The caller must propagate this as ALP_ERR_NOSUPPORT (see the
 *          file header) rather than call this at all if it already knows
 *          it will ignore a false return. */
inline bool fill_fixed_shape(const int64_t *dims, size_t rank, alp_inference_tensor_t *out)
{
	if (rank > 4) {
		return false;
	}
	/* A dim outside uint16_t is the SAME defect one level down: casting it
	 * would wrap silently and hand the caller a plausible wrong extent.
	 * inference_ort.cpp already rejects this at load time
	 * (src/yocto/inference_ort.cpp, _gather_tensor_info) -- "reject here
	 * instead of lying to the caller" -- but inference_deepx.cpp had no
	 * such check, so a dim above 65535 wrapped and a symbolic/dynamic -1
	 * became 65535. Refuse both here so every caller of this helper is
	 * covered, rather than fixing one backend and leaving its sibling
	 * (the #1646 defect class). Checked BEFORE any write to *out. */
	for (size_t i = 0; i < rank; ++i) {
		if (dims[i] < 0 || dims[i] > UINT16_MAX) {
			return false;
		}
	}
	out->rank = static_cast<uint8_t>(rank);
	for (size_t i = 0; i < rank; ++i) {
		out->shape[i] = static_cast<uint16_t>(dims[i]);
	}
	for (size_t i = rank; i < 4; ++i) {
		out->shape[i] = 0;
	}
	return true;
}

} /* namespace alp_inference_shape */

#endif /* ALP_SDK_YOCTO_INFERENCE_TENSOR_SHAPE_H */
