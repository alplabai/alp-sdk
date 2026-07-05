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
# Derive the GET_DIAG_INFO.fw_version marker from firmware-version.txt (the single
# source of truth), exactly as the firmware CMake/ti builds do, so the test asserts
# against the DERIVED constant rather than a drifting literal.  Pre-1.0 packing:
# (MINOR << 8) | PATCH -> 0.2.0 = 0x0200.
file(STRINGS ${BRIDGE_DIR}/firmware-version.txt CC3501E_BRIDGE_FW_VERSION LIMIT_COUNT 1)
if(NOT CC3501E_BRIDGE_FW_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "firmware-version.txt not SemVer: '${CC3501E_BRIDGE_FW_VERSION}'")
endif()
math(EXPR CC3501E_BRIDGE_FW_VERSION_U16 "(${CMAKE_MATCH_2} << 8) | ${CMAKE_MATCH_3}" OUTPUT_FORMAT HEXADECIMAL)
target_compile_definitions(app PRIVATE
    CC3501E_BRIDGE_FW_VERSION_U16=${CC3501E_BRIDGE_FW_VERSION_U16})

# Firmware src + hal, plus the repo-root include/ so the firmware picks up
# the CANONICAL <alp/protocol/cc3501e.h> directly (no mirror).
target_include_directories(app PRIVATE
    ${BRIDGE_DIR}/src
    ${BRIDGE_DIR}/hal
    ${BRIDGE_DIR}/../../include)
