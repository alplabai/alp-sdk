# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_chips_test LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    src/fake_lsm6dso.c
    src/fake_ssd1306.c
    src/fake_bme280.c
)
