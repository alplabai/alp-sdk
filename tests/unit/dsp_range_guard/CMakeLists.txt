# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_dsp_range_guard)

# The guard under test is a dependency-free static-inline in src/dsp_range.h
# (the same header dsp_dispatch.c's CMSIS stats path uses), so the test needs
# only that header -- no DSP backend, no CMSIS build, no mocked kernels.
target_sources(app PRIVATE src/test_dsp_range.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
