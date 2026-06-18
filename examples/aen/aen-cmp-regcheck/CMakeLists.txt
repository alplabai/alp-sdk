# SPDX-License-Identifier: Apache-2.0
#
# On-silicon HSCMP (analog comparator) validation for the E1M-AEN801 (Alif
# Ensemble E8), driving cmp0@49023000 against the on-chip DAC6 INTERNAL reference
# through the portable Zephyr comparator_* class API over the alp-sdk Tier-2
# comparator_alif driver.  A RAM console carries the PASS/FAIL line; the human
# ALSO reads cmp0 CMP_STATUS / CMP_COMP_REG1 over J-Link to confirm the
# comparator is live independently of any printk.
#
# Internal-reference SMOKE: no external wiring needed (the minus terminal is the
# DAC6 reference).  Comparing against an external pad/threshold is bench config
# and is TBD (see README).
#
# Standalone Zephyr app (no alp_project.py board.yaml flow): build with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
#     -DEXTRA_DTC_OVERLAY_FILE=<this-dir>/boards/alp_e1m_aen801_m55_he.overlay

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_cmp_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
