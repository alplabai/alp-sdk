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
 * inference_drpai.cpp's open() resolves the DRP-AI arena through
 * `::open("/dev/drpai0")` + `::ioctl()` BEFORE it touches this class.
 * The fake linux/drpai.h next to this file intercepts both: by default
 * the device is absent (ENOENT, as on a CI host); drpai_test::g_device_present
 * (drpai_test_seam.h) lets a test reach LoadModel()/Run().  The seam also
 * records LoadModel start addresses and detects overlapping Run() calls.
 * See inference_drpai_regression.cpp for what is covered.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "drpai_test_seam.h"

enum class InOutDataType { FLOAT32, FLOAT16, INT32, INT64, OTHER };

class MeraDrpRuntimeWrapper
{
  public:
	MeraDrpRuntimeWrapper() = default;

	bool LoadModel(const std::string &model_dir, uint64_t start_address)
	{
		(void)model_dir;
		drpai_test::g_load_starts.push_back(start_address);
		last_address_ = start_address + drpai_test::g_model_bytes;
		return true;
	}

	/* End of the model placed by LoadModel(), like the real wrapper. */
	uint64_t GetLastAddress() const
	{
		return last_address_;
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

	/* Records how many Run()s are in flight at once; the SDK must never
	 * let that exceed 1 (DRP-AI is one job at a time). */
	void Run()
	{
		const int now  = ++drpai_test::g_in_flight;
		int       prev = drpai_test::g_max_in_flight.load();
		while (now > prev && !drpai_test::g_max_in_flight.compare_exchange_weak(prev, now)) {
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		++drpai_test::g_runs;
		--drpai_test::g_in_flight;
	}

  private:
	uint64_t last_address_ = 0;
};
