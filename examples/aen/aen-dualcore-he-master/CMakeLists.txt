# SPDX-License-Identifier: Apache-2.0
#
# aen-dualcore-he-master -- the HE-master -> HP-peer direction of B1 Option B,
# self-contained (unlike aen-dualcore-master, which is symmetric and needs the
# separate aen-dualcore-probe app hand-paired in as the HP peer). Built TWICE:
# the HE board target is the master role (src/main.c calls
# alp_mproc_boot_core(ALP_CORE_M55_HP, ...) via the deferred-TOC path -- see
# README.md for why this direction can't use the plain 501 release path); the
# HP board target is the peer role (a passive SRAM0 beacon, like
# aen-dualcore-probe). One source file, role picked by CONFIG_BOARD_* at
# compile time, same pattern as every other aen-dualcore-* example.
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_dualcore_he_master LANGUAGES C)
target_sources(app PRIVATE src/main.c)
