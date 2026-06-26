# SPDX-License-Identifier: Apache-2.0
#
# On-silicon CRC-engine validation for the E1M-AEN801 (Alif Ensemble E8),
# computing a known CRC over a fixed compile-time buffer through the PORTABLE
# Zephyr CRC class API (crc_begin/update/finish) over the alp-sdk clean-room
# crc_alif driver, and comparing it against a host-precomputed reference value.
# A RAM console carries the PASS/FAIL line; the human can ALSO read the CRC_OUT
# register over J-Link to confirm the hardware result independently of printk.
#
# The board overlay boards/alp_e1m_aen801_m55_he.overlay auto-applies for the
# matching board target (Zephyr's per-board overlay convention).
#
# Standalone Zephyr app (no alp_project.py board.yaml flow): build with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_crc_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
