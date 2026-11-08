# SPDX-License-Identifier: Apache-2.0
#
# Register-model native_sim test for can_cast_send()'s state checks
# (alp-sdk #2983).  #includes the real zephyr/drivers/can/can_cast.c (no DT
# instances: CONFIG_CAN only) and drives it against an mmap'd register page.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_can_cast_send_errwarn)

get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fake_include
    ${ALP_SDK_ROOT}/zephyr/drivers/can
)
target_sources(app PRIVATE src/test_can_cast_send_errwarn.c)

# Kconfig symbols of the real (Alif-SoC-only) driver, supplied as plain macros.
target_compile_definitions(app PRIVATE
    CONFIG_CAN_MAX_FILTER=4
    CONFIG_CAN_FD_MODE=0
    CONFIG_CAN_ACCEPT_RTR=1
    CONFIG_CAN_RBUF_AFWL=4
    CONFIG_CAN_INIT_PRIORITY=80
)
