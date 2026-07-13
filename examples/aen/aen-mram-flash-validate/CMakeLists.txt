# SPDX-License-Identifier: Apache-2.0
#
# On-silicon MRAM flash write/erase/readback validation for the E1M-AEN801
# (Alif Ensemble E8, M55-HE), exercising the alp-sdk Tier-2 flash driver
# zephyr/drivers/flash/flash_mram_alif.c ("alif,mram-flash-controller") through
# the PORTABLE Zephyr flash_* class API.
#
# Closes the #512 gap: MCUboot/app boot already prove MRAM read+execute, but the
# driver's erase -> program -> readback path had no dedicated bench PASS gate.
# This app drives that path at runtime against the SAFE `storage` partition
# (label "storage", MRAM 0x80560000, 128 KiB NVS/settings) -- NOT slot0/slot1 --
# so the canonical person_detect boot image is never touched.
#
# Bench flow D (MRAM slot0, SES-booted): the SES boots the resident slot0 image
# in preference to a Flow-C ITCM RAM-run, so this app is flashed into MRAM slot0
# (CONFIG_USE_DT_CODE_PARTITION=y -> reset vector 0x8001xxxx) and SE-booted.  At
# runtime it exercises the flash driver against the safe storage partition; the
# ONLY persistent bytes it mutates are inside that partition.  A RAM console
# carries the PASS/FAIL line; the human ALSO reads mem32 0x80560000 over J-Link
# to confirm the written pattern landed in silicon, independent of any printk.
# The canonical person_detect slot0 is restored after the run.
#
# Standalone Zephyr app (no alp_project.py board.yaml flow): build with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
# then flash with scripts/bench/aen/flash-jlink-mramxip.sh.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_mram_flash_validate LANGUAGES C)

target_sources(app PRIVATE src/main.c)
