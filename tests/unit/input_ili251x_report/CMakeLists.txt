# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_input_ili251x_report)

# The logic under test is the dependency-light static-inline
# ili251x_parse_contact0() in zephyr/drivers/input/input_ili251x_report.h --
# the same header input_ili251x.c's ili251x_process() calls to decode the
# 0x10 touch-data report -- so the test needs only stdint/stdbool and
# <zephyr/sys/byteorder.h>, no I2C bus or devicetree-instantiated device.
target_sources(app PRIVATE src/test_input_ili251x_report.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/input)
