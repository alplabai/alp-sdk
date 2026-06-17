# SPDX-License-Identifier: Apache-2.0
#
# On-silicon LPRTC validation for the E1M-AEN801 (Alif Ensemble E8), reading the
# always-on lprtc@42000000 (Synopsys DesignWare APB RTC) through the portable
# Zephyr counter_* class API over the alp-sdk Tier-2 counter_dw_rtc driver.  A
# RAM console carries the PASS/FAIL line; the human ALSO reads the LPRTC CCVR
# register over J-Link to confirm the count advances independently of any printk.
#
# This is a COUNTER regcheck, NOT a calendar-RTC test: the DW-APB-RTC has no
# date/time registers, so it cannot back alp_rtc_* as-is (see README -- the
# counter->calendar shim is TBD).
#
# Standalone Zephyr app (no alp_project.py board.yaml flow): build with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
#     -DEXTRA_DTC_OVERLAY_FILE=<this-dir>/boards/alp_e1m_aen801_m55_he.overlay

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_rtc_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
