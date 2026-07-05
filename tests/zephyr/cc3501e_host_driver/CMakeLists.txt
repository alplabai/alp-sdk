# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_cc3501e_host_driver)

# Link the REAL Alif-side host driver (chips/cc3501e/cc3501e.c), so the request
# encoding + reply decoding under test are the production code paths.  The test
# supplies its own alp_spi_transceive() (a software model of the firmware slave)
# plus inert alp_delay_*/alp_gpio_* seams, so nothing but the driver + the wire
# contract is exercised -- no SDK backend, no silicon.
set(CHIPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../chips)

target_sources(app PRIVATE
    src/test_host_driver.c
    ${CHIPS_DIR}/cc3501e/cc3501e.c
)
# The repo-root include/ gives the driver the CANONICAL public headers
# (<alp/chips/cc3501e.h>, <alp/protocol/cc3501e.h>, <alp/peripheral.h>).
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
