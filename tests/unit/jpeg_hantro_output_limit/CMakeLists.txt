# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_jpeg_hantro_output_limit)

# The guard under test is the dependency-free static-inline
# jpeg_hantro_vc9000e_output_limit() in
# zephyr/drivers/video/jpeg_hantro_vc9000e_limit.h -- the same helper
# jpeg_hantro_vc9000e.c's jpeg_start_encode() calls to program SWREG9 -- so
# the test needs only that one header, no DEVICE_MMIO-backed instance or
# devicetree.
target_sources(app PRIVATE src/test_jpeg_hantro_output_limit.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/video)
