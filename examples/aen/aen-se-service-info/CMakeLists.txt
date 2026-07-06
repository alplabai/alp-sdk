# SPDX-License-Identifier: Apache-2.0
#
# aen-se-service-info -- scopeless on-silicon validation of the Secure Enclave
# (SE) SERVICE transport (the Arm MHUv2 mailboxes + the Apache-2.0 hal_alif
# se_services client) + a zero-risk SoC lifecycle-state (LCS) read on the
# E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run + RAM-console flow.
#
# DELIBERATELY VENDOR-SPECIFIC: this is the sanctioned bring-up escape hatch
# (it pokes the vendor transport by design).  Customer code uses the portable
# wrappers instead -- see aen-se-service-query + src/main.c's header note.
#
# Plain prj.conf app (no alp_project.py board.yaml emit): the SE-service stack
# Kconfigs (CONFIG_ARM_MHUV2 / CONFIG_HAS_ALIF_SE_SERVICES, the alp-sdk symbols
# that compile the in-tree MHUv2 IPM driver + hal_alif's se_service.c) are
# sourced under CONFIG_ALP_SDK, which prj.conf sets, and enabled by the overlay
# (which okays the two MHUv2 nodes + the se_service root node).  See src/main.c
# + the board overlay.

cmake_minimum_required(VERSION 3.20)

# Apply the board overlay explicitly (set BEFORE find_package(Zephyr)).  Zephyr's
# automatic boards/<name>.overlay pickup keys off the FULLY-QUALIFIED board name
# (alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he), not the bare board name, so
# point at the file directly -- this builds cleanly with a plain `west build -b
# alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`.
set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he.overlay")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_se_service_info LANGUAGES C)

target_sources(app PRIVATE src/main.c)
