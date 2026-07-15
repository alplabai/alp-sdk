# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_checked_arith)

# The helpers under test are dependency-free static-inline functions in
# src/common/alp_checked_arith.h (#743), so the test needs only that
# header -- no storage/dsp backend, no dispatcher, no mocked HW.
target_sources(app PRIVATE src/test_checked_arith.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
