# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_flash_mram_range)

# The guard under test is the dependency-free static-inline
# flash_mram_range_is_valid() in zephyr/drivers/flash/flash_mram_range.h --
# the same header flash_mram_alif.c's flash_range_is_valid() calls -- plus
# the shared alp_size_range_valid() it delegates to
# (src/common/alp_checked_arith.h, #743). No DEVICE_MMIO/cmsis_core/
# devicetree needed.
target_sources(app PRIVATE src/test_flash_mram_range.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/flash
	${CMAKE_CURRENT_SOURCE_DIR}/../../../src/common)
