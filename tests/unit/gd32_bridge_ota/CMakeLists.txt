# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_gd32_bridge_ota)

# Link the REAL firmware OTA state machine (partitioned path armed) so the
# offset/length bounds checks under test are the production code paths.  The
# flash read/program seams are redirected into a host buffer via the weak
# ota_fmc_* overrides in the test source, so no raw 32-bit flash address is
# dereferenced on the 64-bit host.
# #1370: the GD32 bridge firmware is moving to its own repository.  Honour an
# ALP_GD32_BRIDGE_FIRMWARE_DIR override so this test follows it there; the
# default is the in-tree copy, so this is a no-op today.
if(NOT DEFINED ALP_GD32_BRIDGE_FIRMWARE_DIR)
    set(ALP_GD32_BRIDGE_FIRMWARE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../firmware/gd32-bridge)
endif()
set(BRIDGE_DIR ${ALP_GD32_BRIDGE_FIRMWARE_DIR})

target_sources(app PRIVATE
    src/test_ota.c
    ${BRIDGE_DIR}/src/ota.c
    ${BRIDGE_DIR}/src/crc32.c
)
target_include_directories(app PRIVATE ${BRIDGE_DIR}/src ${BRIDGE_DIR}/hal)
target_compile_definitions(app PRIVATE BRIDGE_OTA_PARTITIONED)
