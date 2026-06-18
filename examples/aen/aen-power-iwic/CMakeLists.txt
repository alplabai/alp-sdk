# SPDX-License-Identifier: Apache-2.0
#
# Stage-B PM deep-sleep (suspend-to-idle / IWIC) for the E1M-AEN801 (Alif
# Ensemble E8, Cortex-M55-HE).
#
# Brings a minimal Alif PM up over the pinned Zephyr 4.4.0 tree -- which ships no
# Alif pm_state_set and does not select HAS_PM -- and proves CONFIG_PM
# suspend-to-idle (deep IWIC sleep): the kernel idle path selects
# PM_STATE_SUSPEND_TO_IDLE, pm_state_set() (src/alif_pm_iwic.c) programs the AON
# WICCONTROL register (0x1A604010, bit8 WIC / bit9 IWIC) and parks the core in
# WFI; SysTick wakes it.  A wake beacon/heartbeat in global SRAM0 (0x02000000)
# advances each round.
#
# The HAS_PM select lives in this app's Kconfig; the suspend_idle power-state +
# cpu-power-states live in the board overlay.  No edit to upstream zephyr/.
#
# Standalone Zephyr app (no alp_project.py board.yaml flow): build with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"

cmake_minimum_required(VERSION 3.20)

# Apply the board overlay explicitly (set BEFORE find_package(Zephyr)).  Zephyr's
# automatic boards/<name>.overlay pickup keys off the FULLY-QUALIFIED board name
# (alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he), not the bare board name, so
# point at the file directly -- this builds cleanly with a plain `west build -b
# alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` (otherwise the ITCM RAM-run
# retarget is dropped and the image links into MRAM).
set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he.overlay")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_power_iwic LANGUAGES C)

target_sources(app PRIVATE
	src/main.c
	src/alif_pm_iwic.c
)
