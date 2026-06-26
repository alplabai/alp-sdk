# SPDX-License-Identifier: Apache-2.0
#
# On-silicon GPIO validation for the E1M-AEN801 (Alif Ensemble E8), driving a
# safe GPIO pin (P8_0 / gpio8 pin 0) high then low through the upstream
# DesignWare gpio_dw driver (Tier-1).  A RAM console carries the PASS/FAIL line;
# the human ALSO reads the DesignWare DR/DDR registers over J-Link to confirm.
#
# Standalone Zephyr app (no alp_project.py board.yaml flow): build with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
#     -DEXTRA_DTC_OVERLAY_FILE=<this-dir>/boards/alp_e1m_aen801_m55_he.overlay

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_gpio_bench LANGUAGES C)

target_sources(app PRIVATE src/main.c)
