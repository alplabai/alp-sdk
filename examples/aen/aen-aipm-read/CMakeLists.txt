# SPDX-License-Identifier: Apache-2.0
#
# aen-aipm-read -- query the SE for the live aiPM RUN / OFF power+clock profiles
# (run_profile_t / off_profile_t: clock source LFRC/LFXO/HFRC/HFXO/PLL, CPU
# frequency selector, retained memory_blocks, on power_domains, DC-DC mV) and
# DECODE each numeric selector into a human-readable legend, on the
# E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run + RAM-console flow.
#
# This is the aiPM-focused companion to aen-se-service-query: same proven,
# READ-ONLY SE mailbox path, but it leans on the two aiPM getters
# (se_service_get_run_cfg / se_service_get_off_cfg) and turns the raw enum
# values they return into named clock/freq/domain/memory output.  ONLY
# non-mutating queries are called; set_run_cfg / set_off_cfg are deliberately
# never invoked (a wrong power/clock value can brown out the rail) -- see
# src/main.c.
#
# Plain prj.conf app (no alp_project.py board.yaml emit): the SE-service stack
# Kconfigs (CONFIG_ARM_MHUV2 / CONFIG_HAS_ALIF_SE_SERVICES -- the alp-sdk
# symbols that compile the in-tree MHUv2 IPM driver + hal_alif's se_service.c)
# are sourced under CONFIG_ALP_SDK, which prj.conf sets, and the DT nodes are
# enabled by the board overlay.  See src/main.c + the board overlay.

cmake_minimum_required(VERSION 3.20)

# Apply the board overlay explicitly (set BEFORE find_package(Zephyr)).  Zephyr's
# automatic boards/<name>.overlay pickup keys off the FULLY-QUALIFIED board name
# (alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he), not the bare board name, so
# point at the file directly -- this builds cleanly with a plain `west build -b
# alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`.
set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he.overlay")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_aipm_read LANGUAGES C)

target_sources(app PRIVATE src/main.c)
