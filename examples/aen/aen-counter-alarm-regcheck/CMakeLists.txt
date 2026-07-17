# SPDX-License-Identifier: Apache-2.0
#
# On-silicon UTIMER COMPARE-A *alarm* validation for the E1M-AEN801 (Alif
# Ensemble E8).  The sibling aen-counter-utimer-regcheck proves the free-running
# counter advances; this app proves the COMPARE-A one-shot alarm callback fires
# (and re-arms) through the portable Zephyr counter_set_channel_alarm path -- the
# regression for the BUF1-shadow fix in counter_alif_utimer.c.  A RAM console
# carries the PASS/FAIL line.
#
# Standalone Zephyr app (no alp_project.py board.yaml flow): build with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
#     -DEXTRA_DTC_OVERLAY_FILE=<this-dir>/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_counter_alarm_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
