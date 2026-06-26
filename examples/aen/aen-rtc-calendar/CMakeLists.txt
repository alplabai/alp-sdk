# SPDX-License-Identifier: Apache-2.0
#
# On-silicon calendar-RTC validation for the E1M-AEN801 (Alif Ensemble E8),
# exercising the PORTABLE alp_rtc_* calendar surface over the always-on LPRTC
# COUNTER (lprtc@42000000, "snps,dw-apb-rtc") via the alp-sdk
# "lprtc_calendar_shim" rtc backend.  A RAM console carries the PASS/FAIL line;
# the human ALSO reads the LPRTC CCVR register over J-Link to confirm the count
# advances independently of any printk.
#
# The overlay is named for the fully-qualified board (boards/<board>.overlay) so
# it auto-applies; alp-sdk is added as an EXTRA module so the alp_rtc_*
# dispatcher + the E8 calendar shim backend link in.
#
# Standalone Zephyr app (no alp_project.py board.yaml flow): build with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_rtc_calendar LANGUAGES C)

target_sources(app PRIVATE src/main.c)
