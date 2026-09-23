# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_isp_frame_size)

# The helper under test is the dependency-free static-inline
# alp_isp_frame_size() in src/backends/camera/isp_frame_size.h -- needs only
# <zephyr/drivers/video.h> for video_bits_per_pixel(), no DT/MMIO/real ISP.
target_sources(app PRIVATE src/test_isp_frame_size.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../src/backends/camera)
