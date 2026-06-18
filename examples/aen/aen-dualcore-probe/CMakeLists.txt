# SPDX-License-Identifier: Apache-2.0
#
# aen-dualcore-probe -- decisive dual-core boot test for the E1M-AEN801 (E8).
# Built TWICE (RTSS-HE board + RTSS-HP board) into a dual-entry ATOC; each core
# stamps a global-SRAM0 beacon so a SWD read shows whether the SES booted both.
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_dualcore_probe LANGUAGES C)
target_sources(app PRIVATE src/main.c)
