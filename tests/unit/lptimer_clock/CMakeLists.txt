# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_lptimer_clock)

# The bookkeeping under test is the dependency-free static-inline
# counter_alif_lptimer_clock.h the driver's get_value/set_alarm/cancel_alarm/ISR
# call; the test drives it with a fake down-counter (#2944).
target_sources(app PRIVATE src/test_lptimer_clock.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/counter)
