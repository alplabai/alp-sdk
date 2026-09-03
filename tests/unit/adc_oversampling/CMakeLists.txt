# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_adc_oversampling)

# The guard under test is the dependency-free static-inline
# alp_adc_oversampling_ratio_ok() in
# src/backends/adc/adc_oversampling.h -- shared by the Alif E7 / E8
# ADC backends' open() -- so the test needs only <stdbool.h> /
# <stdint.h>, no DT/MMIO/real ADC device.
target_sources(app PRIVATE src/test_adc_oversampling.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../src/backends/adc)
