# SPDX-License-Identifier: Apache-2.0
#
# M55-HP (RTSS-HP) first-light smoke for the E1M-AEN801 (Alif Ensemble E8) -- the
# first alp-sdk image on the SECOND M55.  SES-booted as an M55_HP ATOC
# (loadAddress 0x50000000); proves the HP core runs by writing an advancing
# liveness beacon to global SRAM0 (read over the system AXI-AP).
#
# Standalone Zephyr app: build for the HP board target with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
#     -DEXTRA_DTC_OVERLAY_FILE=<this-dir>/boards/alp_e1m_aen801_m55_hp.overlay

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_hp_core_smoke LANGUAGES C)

target_sources(app PRIVATE src/main.c)
