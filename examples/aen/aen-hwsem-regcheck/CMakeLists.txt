# SPDX-License-Identifier: Apache-2.0
#
# On-silicon HWSEM (hardware-semaphore) validation for the E1M-AEN801 (Alif
# Ensemble E8).  Drives the alp-sdk in-tree "alif,hwsem" driver
# (zephyr/drivers/misc/alif_hwsem/hwsem_alif.c) through its take/give API to
# prove a HWSEM latch acquires and releases on silicon.  A RAM console carries
# the PASS/FAIL line; the human ALSO reads the HWSEM REL register over J-Link to
# confirm the count tracks take (->1) and give (->0).
#
# This is a SINGLE-CORE take/give smoke (a valid acquire/release proof on one
# core); the fuller test is DUAL-CORE arbitration (HE takes, HP must see -EBUSY,
# then acquires after HE gives) -- noted in the README, not driven here.
#
# Standalone Zephyr app (no alp_project.py board.yaml flow): build with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
# The board overlay (boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay) auto-applies by the
# fully-qualified board name -- no DTC_OVERLAY_FILE needed.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_hwsem_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
