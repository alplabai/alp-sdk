# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_cc3501e_transport_lock)

# Link the REAL Alif-side host driver (chips/cc3501e/cc3501e_*.c), so the
# cc3501e_request() transport lock under test (issue #1116) is the
# production code path -- sibling of tests/zephyr/cc3501e_host_driver, but
# this suite drives TWO Zephyr threads concurrently against one shared
# cc3501e_t against a software model of the (single, physical, CS-less)
# firmware SPI slave, instead of one thread against a canned reply set.
set(CHIPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../chips)
file(GLOB CC3501E_DRIVER_SOURCES ${CHIPS_DIR}/cc3501e/cc3501e_*.c)

target_sources(app PRIVATE
    src/test_transport_lock.c
    ${CC3501E_DRIVER_SOURCES}
)
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
