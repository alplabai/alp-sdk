# SPDX-License-Identifier: Apache-2.0
#
# aen-ospi-regcheck -- compile + DT-bind proof of the Alif Ensemble OSPI/HexSPI
# controller (Synopsys DesignWare OSPI, zephyr/drivers/flash/flash_ospi_alif.c,
# compatible "snps,designware-ospi") staging on the E1M-AEN801 and E1M-AEN803
# (both Ensemble E8, M55-HE), via the bench RAM-run + RAM-console flow.
#
# Plain prj.conf app (no alp_project.py board.yaml emit). BUILD-ONLY: this
# proves the ospi0 node BINDS + alif_hal_ospi_initialize() compiles, links,
# and the register file reads back live -- not a live XiP read (XiP setup is
# a deliberate SKIP: OSPI_XIP_SER does not exist on AE822, independent of
# what's populated on the bus -- see src/main.c's CORRECTED PREMISE note,
# #2041) and not a device-level transfer of any kind (controller-register
# proof only). See src/main.c + the board overlay.

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
