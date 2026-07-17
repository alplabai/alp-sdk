# SPDX-License-Identifier: Apache-2.0
#
# Stage-A low-power smoke for the E1M-AEN801 (Alif Ensemble E8, Cortex-M55-HE).
#
# Proves the CPU can enter and leave the architectural Cortex-M55 WFI sleep
# (a periodic k_timer wake + k_cpu_idle) and that the SysTick system clock keeps
# the kernel alive across the sleep -- a wake "beacon"/heartbeat in global SRAM0
# (0x02000000) advances between idles.  This is the WFI-equivalent baseline that
# the Stage-B board pm_state_set IWIC hook (suspend-to-idle / deep IWIC sleep)
# builds on once the Tier-1.5 PM plumbing (HAS_PM + power-states DT + WICCONTROL
# hook) lands.
#
# Stage-A is deliberately CONFIG_PM-FREE: pinned upstream Zephyr 4.4.0's Alif E8
# SoC does NOT `select HAS_PM` (only the alifsemi/zephyr_alif FORK does), so
# CONFIG_PM cannot be enabled over the pinned tree, and upstream provides no
# pm_state_set for the Alif SoC.  Stage-A therefore exercises only the
# arch-level WFI via the idle path + k_cpu_idle(), which needs no PM subsystem
# and builds today.
#
# Standalone Zephyr app (no alp_project.py board.yaml flow): build with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
#     -DEXTRA_DTC_OVERLAY_FILE=<this-dir>/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_power_smoke LANGUAGES C)

target_sources(app PRIVATE src/main.c)
