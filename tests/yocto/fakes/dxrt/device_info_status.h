/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only stand-in for DEEPX dx_rt's dxrt/device_info_status.h.
 * NOT vendor source -- see datatype.h in this directory for why.  Declares
 * only the DeviceStatus calls alp_inference_deepx_get_status() makes.
 */
#pragma once

#include <cstdint>

namespace dxrt
{

class DeviceStatus
{
  public:
	static DeviceStatus GetCurrentStatus(int /*device_id*/)
	{
		return DeviceStatus();
	}
	int Temperature(int /*ch*/) const
	{
		return 0;
	}
	uint32_t NpuClock(int /*ch*/) const
	{
		return 0;
	}
	uint32_t Voltage(int /*ch*/) const
	{
		return 0;
	}
	uint64_t MemorySize() const
	{
		return 0;
	}
};

} /* namespace dxrt */
