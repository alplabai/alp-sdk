# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_gd32_swd_connect_under_reset)

# Links the REAL chips/gd32_swd/gd32_swd.c WITHOUT the alp-sdk Zephyr module
# (this prj.conf omits CONFIG_ALP_SDK).  src/test_gd32_swd_connect_under_reset.c
# supplies the three GPIO handles the driver bit-bangs (alp_gpio_configure /
# write / read) and a bit-level SW-DP target behind them, and logs every pad
# configuration, NRST edge, session notification and target memory write, so
# the ORDER of the connect-under-reset sequence can be asserted.
set(SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

target_sources(app PRIVATE
    src/test_gd32_swd_connect_under_reset.c
    ${SDK_DIR}/chips/gd32_swd/gd32_swd.c
)
target_include_directories(app PRIVATE ${SDK_DIR}/include)
