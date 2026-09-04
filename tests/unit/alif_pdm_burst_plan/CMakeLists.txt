# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_alif_pdm_burst_plan)

# The planner under test is the dependency-free static-inline
# pdm_plan_burst_chunks() in zephyr/drivers/audio/alif_pdm_burst_plan.h --
# the same header alif_pdm.c's ISR rollover calls -- so the test needs only
# stdint/stddef, no DEVICE_MMIO/k_mem_slab/devicetree.
target_sources(app PRIVATE src/test_alif_pdm_burst_plan.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/audio)
