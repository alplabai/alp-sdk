# SPDX-License-Identifier: Apache-2.0
#
# aen-isp-regcheck -- scopeless on-silicon validation of the Alif ISP-Pico
# (Verisilicon ISP Nano "Pico", zephyr/drivers/video/isp_pico.c, compatible
# "vsi,isp-pico") staging on the E1M-AEN801 (Ensemble E8, M55-HE), via the bench
# RAM-run + RAM-console flow.
#
# Plain prj.conf app (no alp_project.py board.yaml emit).  This regcheck does NOT
# enable CONFIG_VIDEO_ISP_VSI -- the isp_pico.c driver TU links the hal_alif
# libisp wrapper, and the locally vendored 2025 wrapper is API-incompatible with
# the 2026 isp_pico.c (missing <zephyr/drivers/video/isp-vsi.h>; 2-arg vs 3-arg
# isp_vsi_bottom_half), so the driver does not compile+link against it (a known
# EXTERNAL hal_alif blocker).  So this app validates the DT BIND of the
# isp node WITHOUT the driver, and reports the wrapper blocker at runtime.
# See src/main.c + the board overlay.

cmake_minimum_required(VERSION 3.20)

# Apply the board overlay explicitly (set BEFORE find_package(Zephyr)).  Zephyr's
# automatic boards/<name>.overlay pickup keys off the FULLY-QUALIFIED board name
# (alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he), not the bare board name, so
# point at the file directly -- this builds cleanly with a plain `west build -b
# alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` (otherwise the ITCM RAM-run
# retarget is dropped and the image links into MRAM).
set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he.overlay")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_isp_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
