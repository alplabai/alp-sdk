# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_isp_pico_tpg_geometry)

# The guard under test is the dependency-free static-inline
# isp_tpg_geometry_is_valid() in zephyr/drivers/video/isp_pico.h -- the same
# guard isp_configure()'s TPG branch calls -- so the test needs only that
# header, no DEVICE_MMIO-backed instance, devicetree, or the hal_alif libisp
# link. isp_pico.h itself #includes upstream Zephyr's own "video_ctrls.h"
# (zephyr/drivers/video/video_ctrls.h, a private header the isp_ctrls struct
# needs), which lives outside our tree -- add ZEPHYR_BASE's drivers/video to
# the include path so that quoted include still resolves.
target_sources(app PRIVATE src/test_isp_pico_tpg_geometry.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/video
	$ENV{ZEPHYR_BASE}/drivers/video)
