# SPDX-License-Identifier: Apache-2.0
#
# aen-ospi-regcheck -- compile + DT-bind proof of the Alif Ensemble OSPI/HexSPI
# controller (Synopsys DesignWare OSPI, zephyr/drivers/flash/flash_ospi_alif.c,
# compatible "snps,designware-ospi") staging on the E1M-AEN801 and E1M-AEN803
# (both Ensemble E8, M55-HE), plus the AEN803 live JEDEC-ID bench target.
#
# Plain prj.conf app (no alp_project.py board.yaml emit). See src/main.c and
# the per-SKU overlays for the deliberately different proof contracts.

cmake_minimum_required(VERSION 3.20)

# No DTC_OVERLAY_FILE: Zephyr applies boards/<fully-qualified-board>.overlay by
# itself -- alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay for
# `-b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`, the aen803 file for the
# AEN803 target -- ITCM retarget included. Pinning the AEN801 file here, as
# this file used to, handed the AEN801 overlay to an AEN803 build and left the
# AEN803 one unread.

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_ospi_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
