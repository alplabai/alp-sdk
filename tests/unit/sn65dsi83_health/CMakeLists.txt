# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_sn65dsi83_health)

# The decision under test is the dependency-free static-inline
# sn65dsi83_health_decide() / sn65dsi83_reinit_allowed() in
# zephyr/drivers/display/sn65dsi83_recovery.h -- the same header the driver and the
# recovery agent call through sn65dsi83_health_poll().  The I2C replay itself is bench
# territory; the test needs no bus.
target_sources(app PRIVATE src/test_sn65dsi83_health.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/display)
