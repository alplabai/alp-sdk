# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_gd32_bridge_transport)

# Link the REAL firmware transport + dispatcher (stub HAL backend), so the
# CS-framing seams under test are the production code paths, not a mock.
set(BRIDGE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../firmware/gd32-bridge)

target_sources(app PRIVATE
    src/test_transport_spi.c
    ${BRIDGE_DIR}/src/transport_spi.c
    ${BRIDGE_DIR}/src/protocol.c
    ${BRIDGE_DIR}/src/bootloader/bootloader.c
    ${BRIDGE_DIR}/src/ota.c
    ${BRIDGE_DIR}/src/crc32.c
    ${BRIDGE_DIR}/hal/bridge_hw_stub.c
)
target_include_directories(app PRIVATE ${BRIDGE_DIR}/src ${BRIDGE_DIR}/hal)
