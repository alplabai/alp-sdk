# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_gd32_bridge_ota_overflow)

# Exercise the REAL OTA state machine (src/ota.c) with BRIDGE_OTA_PARTITIONED
# armed, so the wire-controlled offset/length wrap guard (#470) is the
# production code path, not a mock.
#
# src/ota.c is source-included (not compiled as its own TU) by
# test_ota_overflow.c: h_write()'s wrap guard is `static`, and reaching it
# through the public ota_dispatch() would require BEGIN to walk the A/B
# metadata at its real flash-mapped addresses (OTA_META_REC0/1), which are
# only valid on-silicon -- not in a host/native_sim process.  Source
# inclusion gives the test direct access to the static state + h_write()
# without faking a flash-backed address space.  crc32.c stays a separate TU
# (ota_crc32() is referenced, if unreached, by code paths this test doesn't
# exercise).
set(BRIDGE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../firmware/gd32-bridge)

target_sources(app PRIVATE
    src/test_ota_overflow.c
    ${BRIDGE_DIR}/src/crc32.c
)
target_include_directories(app PRIVATE ${BRIDGE_DIR}/src ${BRIDGE_DIR}/hal)
target_compile_definitions(app PRIVATE BRIDGE_OTA_PARTITIONED=1)
