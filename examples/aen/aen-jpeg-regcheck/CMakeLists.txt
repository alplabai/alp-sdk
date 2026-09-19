# SPDX-License-Identifier: Apache-2.0
#
# aen-jpeg-regcheck -- see testcase.yaml for the two scenarios this app
# builds under (native_sim software fallback / AEN801 hardware backend).

cmake_minimum_required(VERSION 3.20)

# No DTC_OVERLAY_FILE: Zephyr applies boards/<fully-qualified-board>.overlay
# by itself, so the AEN801 and AEN803 targets each read their own file.

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_jpeg_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
