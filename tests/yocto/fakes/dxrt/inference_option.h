/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only stand-in for DEEPX dx_rt's dxrt/inference_option.h.
 * NOT vendor source -- see datatype.h in this directory for why.
 */
#pragma once

#include <cstdint>

namespace dxrt
{

struct InferenceOption {
	/* Which NPU cores the engine runs on (0 = all); set by
	 * alp_inference_deepx_bind_cores(). */
	uint32_t boundOption = 0;
};

inline InferenceOption DefaultInferenceOption;

} /* namespace dxrt */
