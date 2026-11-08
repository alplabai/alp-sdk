# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_gd32_protocol_015)

# Links the REAL chips/gd32g553/gd32g553.c WITHOUT the alp-sdk Zephyr module
# (this prj.conf omits CONFIG_ALP_SDK -- same pattern as
# tests/unit/gd32_link_seq_reset).  src/test_gd32_protocol_015.c supplies the
# SPI bus + the alp_delay_* / alp_uptime_ms primitives and models a GD32
# bridge that answers either the v0.14 or the v0.15 wire protocol, so the
# real driver's negotiation, variable-length reply parsing and ATTN handling
# run unmodified against both.
set(SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

target_sources(app PRIVATE
    src/test_gd32_protocol_015.c
    ${SDK_DIR}/chips/gd32g553/gd32g553.c
)
target_include_directories(app PRIVATE ${SDK_DIR}/include)
