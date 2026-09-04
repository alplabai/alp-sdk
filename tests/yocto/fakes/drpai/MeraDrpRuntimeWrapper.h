/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only stand-in for the EdgeCortix/Renesas DRP-AI TVM application
 * runtime wrapper (rzv_drp-ai_tvm/apps/MeraDrpRuntimeWrapper.h).
 *
 * NOT vendor source.  The real header ships only inside the RZ/V Yocto
 * SDK sysroot (the prebuilt MERA2 runtime libs + DRP-AI Translator are
 * Renesas/EdgeCortix account-gated -- see src/yocto/inference_drpai.cpp's
 * "Vendor-artifact handling" note) and cannot be vendored into this
 * public repo or built in CI (issue #1747: no PR gate builds this
 * backend, same reason the ORT/DEEPX fakes in this directory exist).
 * This declares only the shape inference_drpai.cpp documents itself as
 * using (its own "Real vendor API" header comment), reconstructed from
 * that documentation alone -- clean-room, same treatment
 * tests/yocto/fakes/dxrt/ already gives dx_rt.
 *
 * Unlike the ORT/DEEPX fakes, this one carries no dxrt_test::-style
 * seam: inference_drpai.cpp's open() resolves the DRP-AI reserved-memory
 * arena via a REAL `::open("/dev/drpai0", ...)` + `::ioctl()` (see the
 * fake linux/drpai.h next to this file) BEFORE it ever touches
 * MeraDrpRuntimeWrapper, and a CI runner has no such device -- that call
 * always fails, so open() never reaches LoadModel()/GetInputInfo() in
 * these tests regardless of what this class does. See
 * inference_drpai_regression.cpp's header comment for what IS covered.
 */
#pragma once

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

enum class InOutDataType { FLOAT32, FLOAT16, INT32, INT64, OTHER };

class MeraDrpRuntimeWrapper
{
  public:
	MeraDrpRuntimeWrapper() = default;

	bool LoadModel(const std::string &model_dir, uint64_t start_address)
	{
		(void)model_dir;
		(void)start_address;
		return true;
	}

	void SetInput(int idx, const float *data)
	{
		(void)idx;
		(void)data;
	}

	void SetInput(int idx, const uint16_t *data)
	{
		(void)idx;
		(void)data;
	}

	std::vector<std::tuple<std::string, size_t, InOutDataType>> GetInputInfo()
	{
		return {};
	}

	std::vector<std::tuple<std::string, size_t, InOutDataType>> GetOutputInfo()
	{
		return {};
	}

	std::tuple<InOutDataType, void *, int64_t> GetOutput(int idx)
	{
		(void)idx;
		return std::make_tuple(InOutDataType::FLOAT32, nullptr, static_cast<int64_t>(0));
	}

	void Run()
	{
	}
};
