# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_mjpeg_quality_ladder)

# The function under test, jpeg_quality_step_down(), is a dependency-free
# static-inline in the camera-mjpeg-stream example's own
# jpeg_quality_ladder.h -- no DEVICE_MMIO instance or devicetree needed,
# same reasoning as tests/unit/jpeg_hantro_output_limit.
target_sources(app PRIVATE src/test_mjpeg_quality_ladder.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/connectivity/camera-mjpeg-stream/src)
