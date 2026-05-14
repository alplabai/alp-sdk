# SPDX-License-Identifier: Apache-2.0
#
# Twister smoke test for the §D.lib library-knob path.  Exercises
# the alp.conf -> Kconfig -> Zephyr build chain end-to-end for the
# libraries that live in Zephyr's own modules tree (lvgl / mbedtls /
# cmsis-dsp / tflite-micro).  Catches:
#   - Loader regressions that drop a CONFIG_*=y line.
#   - Zephyr-module include-path breakage.
#   - Cross-config issues (e.g. mbedtls + tflite-micro both pulled
#     in, link-time symbol collisions).
#
# extras-tier1-only libraries (u8g2, opus, libcoap, ...) are NOT
# exercised here because their west pin is disabled by default;
# a follow-up workflow with `--group-filter +extras-tier1` covers
# those once the pins stabilise.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_library_knobs_test LANGUAGES C CXX)

target_sources(app PRIVATE src/main.c)
