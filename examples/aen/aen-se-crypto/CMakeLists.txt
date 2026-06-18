# SPDX-License-Identifier: Apache-2.0
#
# aen-se-crypto -- drive the portable <alp/security.h> compute surface
# (SHA-256 known-answer + AES-128-GCM round-trip + TRNG) on the E1M-AEN801
# (Ensemble E8, M55-HE) backed by the Secure Enclave's CryptoCell.  The
# companion to aen-se-service-query (which proves the READ-ONLY SE surface);
# this app exercises the *compute* services that ride the same MHUv2 -> SE
# mailbox, through the SE-CryptoCell security backend
# (src/backends/security/se_cryptocell.c).
#
# Plain prj.conf app (no alp_project.py board.yaml emit): the SE-service
# stack Kconfigs (CONFIG_ARM_MHUV2 / CONFIG_HAS_ALIF_SE_SERVICES) and the
# SE-CryptoCell backend gate (CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL) are
# sourced under CONFIG_ALP_SDK, which prj.conf sets; the overlay okays the
# MHUv2 nodes + the se_service root node and supplies the itcm/dtcm
# global_base the SE address translation needs.

cmake_minimum_required(VERSION 3.20)

# Apply the board overlay explicitly (set BEFORE find_package(Zephyr)).
# Zephyr's automatic boards/<name>.overlay pickup keys off the
# FULLY-QUALIFIED board name
# (alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he), not the bare board
# name, so point at the file directly -- this builds cleanly with a plain
# `west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`.
set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he.overlay")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_se_crypto LANGUAGES C)

target_sources(app PRIVATE src/main.c)
