/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only control surface shared by the fake linux/drpai.h and the fake
 * MeraDrpRuntimeWrapper.h, and read by inference_drpai_regression.cpp.
 * Not a stand-in for any vendor header.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <cstdint>
#include <vector>

namespace drpai_test
{

/* When false (the default) the fake `::open("/dev/drpai0")` fails with
 * ENOENT, exactly like a CI host: the original device-absent tests keep
 * covering that path.  A test that wants to reach LoadModel()/Run() flips
 * it to true. */
inline bool g_device_present = false;

/* What the fake DRPAI_GET_DRPAI_AREA reports. */
inline uint64_t g_area_base = 0xd0000000ull;
inline uint64_t g_area_size = 0x20000000ull;

/* Bytes the fake runtime "uses" per LoadModel (GetLastAddress = start + this). */
inline uint64_t g_model_bytes = 0x4000000ull;

/* Start addresses LoadModel() was called with, in call order. */
inline std::vector<uint64_t> g_load_starts;

/* Overlapping-Run detector: Run() bumps g_in_flight, sleeps briefly, and
 * records the high-water mark. */
inline std::atomic<int> g_in_flight{ 0 };
inline std::atomic<int> g_max_in_flight{ 0 };
inline std::atomic<int> g_runs{ 0 };

} /* namespace drpai_test */
