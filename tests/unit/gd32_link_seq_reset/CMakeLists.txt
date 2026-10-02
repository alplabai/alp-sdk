# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_gd32_link_seq_reset)

# Links the REAL chips/gd32g553/gd32g553.c WITHOUT the alp-sdk Zephyr module
# (this prj.conf omits CONFIG_ALP_SDK -- same pattern as
# tests/unit/pwm_gd32_bridge_period).  The SPI bus + the alp_delay_* /
# alp_uptime_ms primitives the driver calls are supplied by
# src/test_gd32_link_seq_reset.c, which models a GD32 bridge with the v0.7
# STATUS_SEQ stamp so a slave reset can be simulated on the wire.
set(SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

target_sources(app PRIVATE
    src/test_gd32_link_seq_reset.c
    ${SDK_DIR}/chips/gd32g553/gd32g553.c
)
target_include_directories(app PRIVATE ${SDK_DIR}/include)
