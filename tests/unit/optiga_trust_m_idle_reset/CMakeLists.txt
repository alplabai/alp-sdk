# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_optiga_trust_m_idle_reset)

# Links the REAL chips/optiga_trust_m driver and the vendored Infineon host
# library WITHOUT the alp-sdk Zephyr module (this prj.conf omits CONFIG_ALP_SDK,
# same pattern as tests/unit/gd32_link_seq_reset).  The I2C bus and the
# alp_delay_ms / alp_uptime_ms primitives are supplied by
# src/test_optiga_trust_m_idle_reset.c, which models the part's post-idle
# NACK pattern (#2507).
set(SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
include(${SDK_DIR}/vendors/optiga-trust-m/sources.cmake)

target_sources(app PRIVATE
    src/test_optiga_trust_m_idle_reset.c
    ${SDK_DIR}/chips/optiga_trust_m/optiga_trust_m.c
    ${ALP_OPTIGA_SOURCES}
)
target_include_directories(app PRIVATE ${SDK_DIR}/include ${ALP_OPTIGA_INCLUDE_DIRS})
target_compile_definitions(app PRIVATE ${ALP_OPTIGA_DEFINES})
