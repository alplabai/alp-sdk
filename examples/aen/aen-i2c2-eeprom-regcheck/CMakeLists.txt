# SPDX-License-Identifier: Apache-2.0
#
# On-silicon I2C2 + EEPROM validation for the E1M-AEN801 (Alif Ensemble E8),
# scanning the SoC I2C2 DesignWare master bus and reading the on-module 24C128
# EEPROM @0x50 through the UPSTREAM Zephyr i2c_dw driver (Tier-1).  A RAM console
# carries the scan results + EEPROM hexdump + PASS/FAIL line.
#
# Standalone Zephyr app (no alp_project.py board.yaml flow): build with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
#     -DEXTRA_DTC_OVERLAY_FILE=<this-dir>/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_i2c2_eeprom_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
