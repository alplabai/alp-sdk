# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_sn65dsi83_health)

# Two layers under test, both from zephyr/drivers/display/:
#  - the dependency-free static-inline sn65dsi83_health_decide() / sn65dsi83_reinit_allowed()
#    in sn65dsi83_recovery.h (the decision), and
#  - sn65dsi83_health_poll() in sn65dsi83_recovery.c (read, decide, replay), driven against a
#    fake I2C controller the test defines itself (a register file behind an i2c_driver_api),
#    so no bridge, DSI host or devicetree node is needed.
target_sources(app PRIVATE
	src/test_sn65dsi83_health.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/display/sn65dsi83_recovery.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/display)
