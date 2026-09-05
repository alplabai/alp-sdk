# SPDX-License-Identifier: Apache-2.0
#
# aen-temp-sensor -- read the E1M-AEN801's on-module TMP112 temperature sensor
# through the UPSTREAM Zephyr sensor API (CONFIG_SENSOR + CONFIG_TMP112,
# drivers/sensor/ti/tmp112/), bound to the "ti,tmp112" node the BOARD layer
# declares on BRD_I2C.
#
# Plain Zephyr app -- no alp_project.py board.yaml flow and no alp-sdk module
# needed: every API this example touches is upstream Zephyr (ADR 0017 Tier-1,
# upstream-native), so pulling <alp/*> in would add footprint without adding a
# surface the app uses.  Build with:
#
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
#
# or, on the bench:
#
#   scripts/bench/aen/build.sh <alp-sdk>/examples/aen/aen-temp-sensor
#
# The overlay under boards/ is named for the FULLY-QUALIFIED board id, so
# Zephyr auto-applies it -- no -DEXTRA_DTC_OVERLAY_FILE needed.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_temp_sensor LANGUAGES C)

target_sources(app PRIVATE src/main.c)
