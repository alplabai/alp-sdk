/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only stand-in for DEEPX dx_rt's dxrt/inference_option.h.
 * NOT vendor source -- see datatype.h in this directory for why.
 */
#pragma once

namespace dxrt
{

struct InferenceOption {
	int placeholder = 0;
};

inline InferenceOption DefaultInferenceOption;

} /* namespace dxrt */
