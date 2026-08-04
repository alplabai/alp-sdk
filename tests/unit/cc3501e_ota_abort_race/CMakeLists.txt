# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_cc3501e_ota_abort_race)

# Link the REAL firmware OTA state machine (cc3501e_hw_ti_ota.c, issue #1123)
# so the abort-vs-FINISH chunking under test is the production code path.
# All PSA-FWU access in that file goes through cc3501e_hw_ti_ota_psa.h's
# plain-C seam; the test source provides an in-memory mock of that seam
# instead of linking cc3501e_hw_ti_ota_psa.c's real TI-SDK pass-through, so
# no vendor SimpleLink SDK is needed on native_sim.
set(FW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../firmware/cc3501e)

target_sources(app PRIVATE
    src/test_ota_abort_race.c
    ${FW_DIR}/hal/ti/cc3501e_hw_ti_ota.c
)
target_include_directories(app PRIVATE
    ${FW_DIR}/hal
    ${FW_DIR}/hal/ti
    ${FW_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
