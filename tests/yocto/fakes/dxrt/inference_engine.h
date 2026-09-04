/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only stand-in for DEEPX dx_rt's dxrt/inference_engine.h.
 * NOT vendor source -- see datatype.h in this directory for why.
 *
 * dxrt_test:: below is a seam this stand-in adds that the real dx_rt has
 * no equivalent of: it lets tests/yocto/inference_deepx_regression.cpp
 * control what GetInputs()/GetOutputs() (the open()-time DECLARED tensor
 * metadata) and Run() (the LIVE per-invoke result) hand back, so the
 * test can drive src/yocto/inference_deepx.cpp's own rank-guard logic
 * deterministically -- both the open()-time guard (all_tensor_ranks_fit())
 * and the separate live-path guard in alp_inference_deepx_get_output() --
 * without any real DEEPX device or PCIe link.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "dxrt/inference_option.h"
#include "dxrt/tensor.h"

namespace dxrt_test
{

inline dxrt::Tensors    g_declared_inputs;
inline dxrt::Tensors    g_declared_outputs;
inline dxrt::TensorPtrs g_run_outputs;
inline bool             g_run_should_throw = false;

} /* namespace dxrt_test */

namespace dxrt
{

class InferenceEngine
{
  public:
	InferenceEngine(const uint8_t *buf, std::size_t size, InferenceOption &opt)
	{
		(void)buf;
		(void)size;
		(void)opt;
	}

	Tensors GetInputs()
	{
		return dxrt_test::g_declared_inputs;
	}
	Tensors GetOutputs()
	{
		return dxrt_test::g_declared_outputs;
	}

	TensorPtrs Run(void *input_ptr)
	{
		(void)input_ptr;
		if (dxrt_test::g_run_should_throw) {
			throw std::runtime_error("fake dxrt Run() failure");
		}
		return dxrt_test::g_run_outputs;
	}
};

} /* namespace dxrt */
