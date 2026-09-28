# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_isp_frame_size)

# The helpers under test are the dependency-free static-inline
# alp_isp_pixel_bpp() / alp_isp_default_pitch() / alp_isp_frame_size() in
# zephyr/include/zephyr/drivers/video/isp_frame_size.h -- the SAME header
# zephyr/drivers/video/isp_pico.c and src/backends/camera/alif_isp_pico.c
# include, so this test exercises the exact helper both call, not a
# stand-in copy. Needs only <zephyr/drivers/video.h> for
# video_bits_per_pixel() and the FOURCCs from video_alif.h, no DT/MMIO/real
# ISP.
target_sources(app PRIVATE src/test_isp_frame_size.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/include)
