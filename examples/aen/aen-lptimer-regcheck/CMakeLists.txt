# SPDX-License-Identifier: Apache-2.0
#
# On-silicon LPTIMER validation for the E1M-AEN801 (Alif Ensemble E8), reading
# the always-on lptimer@42001000 (Alif low-power timer, channel 0) through the
# portable Zephyr counter_* class API over the alp-sdk Tier-1.5
# counter_alif_lptimer driver.  A RAM console carries the PASS/FAIL line; the
# human ALSO reads the LPTIMER CURRENTVAL register over J-Link to confirm the
# count advances independently of any printk.
#
# The LPTIMER is a DOWN-counter (free-run loads 0xFFFFFFFF and counts down) and
# is a DISTINCT always-on IP from the LPRTC (aen-rtc-regcheck) and the UTIMER.
#
# Standalone Zephyr app (no alp_project.py board.yaml flow): build with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
#     -DEXTRA_DTC_OVERLAY_FILE=<this-dir>/boards/alp_e1m_aen801_m55_he.overlay

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_lptimer_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
