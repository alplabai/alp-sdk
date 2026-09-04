# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_display_cdc200_bounds)

# The guard under test is the dependency-free static-inline
# cdc200_validate_transfer() in zephyr/drivers/display/display_cdc200.h --
# the same header display_cdc200.c's four read/write entry points call --
# so the test needs only <zephyr/drivers/display.h> plus this header's plain
# cdc200_layer_config struct, no DEVICE_MMIO-backed instance or devicetree.
target_sources(app PRIVATE src/test_display_cdc200_bounds.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/display)
