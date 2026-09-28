# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_alif_pdm_trigger_state)

# The decision under test is the dependency-free static-inline
# pdm_decide_start() in zephyr/drivers/audio/alif_pdm_trigger_state.h -- the
# same header dmic_alif_pdm_trigger()'s DMIC_TRIGGER_START case calls -- so
# the test needs only stdbool, no DEVICE_MMIO/k_msgq/PDM-instance.
target_sources(app PRIVATE src/test_alif_pdm_trigger_state.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/audio)
