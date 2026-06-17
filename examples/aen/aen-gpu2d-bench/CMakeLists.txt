# SPDX-License-Identifier: Apache-2.0
#
# On-silicon GPU2D software-fallback validation for the E1M-AEN801 (Alif
# Ensemble E8).  Exercises the portable <alp/gpu2d.h> surface (fill_rect / blit /
# blend) on the real M55-HE core through the priority-0 pure-C sw_fallback
# backend (the D/AVE 2D HW backend is opt-in + bench-unverified, so a plain
# CONFIG_ALP_SDK build lands on sw_fallback).  No HW dependency -- pure CPU pixel
# work over RAM buffers, checked against exact expected values.  A RAM console
# carries the PASS/FAIL line.
#
# Standalone Zephyr app (no alp_project.py board.yaml flow): build with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
#     -DEXTRA_DTC_OVERLAY_FILE=<this-dir>/boards/alp_e1m_aen801_m55_he.overlay

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_gpu2d_bench LANGUAGES C)

target_sources(app PRIVATE src/main.c)
