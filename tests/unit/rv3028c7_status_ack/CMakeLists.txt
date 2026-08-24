# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_rv3028c7_status_ack)

# The helper under test is a dependency-free static-inline function in
# chips/rv3028c7/rv3028c7_status_ack.h (#1623), so the test needs only
# that header -- no I2C bus, no fake backend, no rv3028c7_t context.
target_sources(app PRIVATE src/test_rv3028c7_status_ack.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../chips/rv3028c7)
