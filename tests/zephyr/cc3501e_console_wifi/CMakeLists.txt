# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_cc3501e_console_wifi)

# Link the REAL `alp companion wifi` command-group TU together with the REAL
# CC3501E host driver, so what is under test is the production print path all
# the way down to the wire encode/decode -- not a re-implementation of it.
#
# The test supplies the two seams that would otherwise need silicon: its own
# alp_spi_transceive() (a software model of the firmware SPI slave, same shape
# as tests/zephyr/cc3501e_host_driver) and the `companion_cc3501e` handle the
# console TU takes as an extern.  No SDK backend, no shell console of the SDK
# -- just a minimal (alp, companion) subcommand root declared in the test.
set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
file(GLOB CC3501E_DRIVER_SOURCES ${REPO_ROOT}/chips/cc3501e/cc3501e_*.c)

target_sources(app PRIVATE
    src/test_console_wifi.c
    ${REPO_ROOT}/src/zephyr/console/alp_console_companion_wifi.c
    ${CC3501E_DRIVER_SOURCES}
)
target_include_directories(app PRIVATE ${REPO_ROOT}/include)
