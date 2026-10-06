# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_adc_gd32_bridge_burst)

# Links the REAL src/backends/adc/gd32_bridge.c WITHOUT the alp-sdk Zephyr
# module (same pattern as tests/unit/pwm_gd32_bridge_period): the supervisor
# acquire/release and gd32g553_adc_* seam are stubbed in src/test_burst.c.
set(SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

target_sources(app PRIVATE
    src/test_burst.c
    ${SDK_DIR}/src/backend.c
    ${SDK_DIR}/src/backends/adc/gd32_bridge.c
)
target_include_directories(app PRIVATE
    ${SDK_DIR}/include
    ${SDK_DIR}/src
    ${SDK_DIR}/src/common
    ${SDK_DIR}/src/backends/adc
    ${SDK_DIR}/src/zephyr
)
