# SPDX-License-Identifier: Apache-2.0
#
# aen-isp-regcheck -- scopeless on-silicon validation of the Alif ISP-Pico
# (Verisilicon ISP Nano "Pico", zephyr/drivers/video/isp_pico.c, compatible
# "vsi,isp-pico") staging on the E1M-AEN801 (Ensemble E8, M55-HE), via the bench
# RAM-run + RAM-console flow.
#
# Plain prj.conf app (no alp_project.py board.yaml emit).  This regcheck enables
# CONFIG_VIDEO_ISP_VSI=y -- the isp_pico.c driver TU now COMPILES + LINKS
# against hal_alif v2.3.0 (isp-vsi.h + isp_ctrl_params.h vendored VERBATIM into
# zephyr/include/zephyr/drivers/video/, see the driver file header). This is a
# BUILD-ONLY proof: RUNTIME stays BENCH-BLOCKED because no camera sensor is
# wired on this hardware batch, so the m2m capture graph never runs. See
# src/main.c + the board overlay.

cmake_minimum_required(VERSION 3.20)

# Apply the board overlay explicitly (set BEFORE find_package(Zephyr)).  Zephyr's
# automatic boards/<name>.overlay pickup keys off the FULLY-QUALIFIED board name
# (alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he), not the bare board name, so
# point at the file directly -- this builds cleanly with a plain `west build -b
# alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` (otherwise the ITCM RAM-run
# retarget is dropped and the image links into MRAM).
set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_isp_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
