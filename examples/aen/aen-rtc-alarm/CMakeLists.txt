# SPDX-License-Identifier: Apache-2.0
#
# On-module RV-3028-C7 RTC + alarm-interrupt example for the E1M-AEN801 (Alif
# Ensemble E8, M55-HE), over UPSTREAM Zephyr's rtc_* API (rtc_rv3028, ADR 0017
# Tier-1 -- no alp-sdk driver in the path).
#
# Plain Zephyr app: it consumes a devicetree-bound upstream driver, so it has no
# board.yaml / alp_project.py step (nothing to select via CONFIG_ALP_SDK_CHIP_*).
# Build with the alp-sdk bench helper, which resolves the fully-qualified board
# target so boards/*.overlay auto-applies:
#
#   scripts/bench/aen/build.sh examples/aen/aen-rtc-alarm
#
# or by hand:
#   west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
#       examples/aen/aen-rtc-alarm -- \
#       "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_rtc_alarm LANGUAGES C)

target_sources(app PRIVATE src/main.c)
