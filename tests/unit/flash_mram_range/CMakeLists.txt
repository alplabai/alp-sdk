# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_flash_mram_range)

# The guard under test is the dependency-free static-inline
# flash_mram_range_is_valid() in zephyr/drivers/flash/flash_mram_range.h --
# the same header flash_mram_alif.c's flash_range_is_valid() calls. The
# subtraction check is duplicated inline there (not #include
# src/common/alp_checked_arith.h) so the header stays resolvable in images
# that never set CONFIG_ALP_SDK (e.g. MCUboot); see that header's banner.
# No DEVICE_MMIO/cmsis_core/devicetree needed.
target_sources(app PRIVATE src/test_flash_mram_range.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/flash)
