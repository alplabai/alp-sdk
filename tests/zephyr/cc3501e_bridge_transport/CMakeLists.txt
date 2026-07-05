# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_cc3501e_bridge_transport)

# Link the REAL firmware transport + dispatcher (stub HAL backend), so the
# framing seams under test are the production code paths, not a mock.
set(BRIDGE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../firmware/cc3501e)

target_sources(app PRIVATE
    src/test_transport_spi.c
    ${BRIDGE_DIR}/src/transport_spi.c
    ${BRIDGE_DIR}/src/transport_sdio.c
    ${BRIDGE_DIR}/src/protocol.c
    ${BRIDGE_DIR}/src/worker.c
    ${BRIDGE_DIR}/src/event_ring.c
    ${BRIDGE_DIR}/hal/cc3501e_hw_stub.c
)
# Firmware src + hal, plus the repo-root include/ so the firmware picks up
# the CANONICAL <alp/protocol/cc3501e.h> directly (no mirror).
target_include_directories(app PRIVATE
    ${BRIDGE_DIR}/src
    ${BRIDGE_DIR}/hal
    ${BRIDGE_DIR}/../../include)
