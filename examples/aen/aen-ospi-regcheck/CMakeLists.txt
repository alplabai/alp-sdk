# SPDX-License-Identifier: Apache-2.0
#
# aen-ospi-regcheck -- controller proof on E1M-AEN801 and live JEDEC-ID bench
# target for the populated E1M-AEN803 OSPI0 CS1 NOR.
#
# Plain prj.conf app (no alp_project.py board.yaml emit). See src/main.c and
# the per-SKU overlays for the deliberately different proof contracts.

cmake_minimum_required(VERSION 3.20)

# Apply the board overlay explicitly (set BEFORE find_package(Zephyr)).  Zephyr's
# automatic boards/<name>.overlay pickup keys off the FULLY-QUALIFIED board name
# (alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he), not the bare board name, so
# point at the file directly -- this builds cleanly with a plain `west build -b
# alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` (otherwise the ITCM RAM-run
# retarget is dropped and the image links into MRAM).
if(BOARD MATCHES "aen803")
  set(DTC_OVERLAY_FILE "boards/alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he.overlay")
else()
  set(DTC_OVERLAY_FILE "boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_ospi_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
