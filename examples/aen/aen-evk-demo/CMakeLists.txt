# SPDX-License-Identifier: Apache-2.0
#
# aen-evk-demo -- phased full-board demo for the E1M-AEN801 (Alif Ensemble
# E8, M55-HE) on the E1M EVK carrier, bench RAM-run via J-Link.
#
# Standalone Zephyr app (no alp_project.py board.yaml flow): every bus this
# app touches (BRD_I2C / SoC I2C0, the carrier's SoC I2C2) is already
# enabled by the BOARD layer (metadata/e1m_modules/aen/on-module-links.yaml,
# metadata/pinmux/aen.yaml) -- see the sibling aen-rtc-control2-probe /
# aen-sensor-int-probe / aen-brd-i2c-scan apps for the same argument made in
# full. Only the RGB LED's three PWM channels need this app's OWN overlay,
# because nothing in the board tree enables PWM today (the pwm_alif_utimer.c
# driver is INTERIM / BENCH-UNVERIFIED -- see its file header). Build with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_evk_demo LANGUAGES C)

target_sources(app PRIVATE src/main.c)
