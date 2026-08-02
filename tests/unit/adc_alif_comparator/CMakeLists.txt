# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_adc_alif_comparator)

# The guard under test is the dependency-free static-inline
# adc_alif_sequence_comparator() in zephyr/drivers/adc/adc_alif_comparator.h
# -- the same header adc_alif.c's adc_start_read() and the CMPA/CMPB IRQ
# handlers use -- so the test needs only <zephyr/drivers/adc.h>, no
# DEVICE_MMIO/analog_ctrl HAL/devicetree.
target_sources(app PRIVATE src/test_adc_alif_comparator.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/adc)
