# SPDX-License-Identifier: Apache-2.0
#
# aen-ospi-regcheck -- compile + DT-bind proof of the Alif Ensemble OSPI/HexSPI
# controller (Synopsys DesignWare OSPI, zephyr/drivers/flash/flash_ospi_alif.c,
# compatible "snps,designware-ospi") staging on the E1M-AEN801 (Ensemble E8,
# M55-HE), via the bench RAM-run + RAM-console flow.
#
# Plain prj.conf app (no alp_project.py board.yaml emit). HW-BLOCKED,
# BUILD-ONLY this batch: no octal-NOR/HyperBus part is populated, so this
# proves the ospi0 node BINDS + the hal_alif init/XiP-enable entry points
# compile and link -- not a live XiP read. See src/main.c + the board overlay.

cmake_minimum_required(VERSION 3.20)

# Apply the board overlay explicitly (set BEFORE find_package(Zephyr)).  Zephyr's
# automatic boards/<name>.overlay pickup keys off the FULLY-QUALIFIED board name
# (alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he), not the bare board name, so
# point at the file directly -- this builds cleanly with a plain `west build -b
# alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` (otherwise the ITCM RAM-run
# retarget is dropped and the image links into MRAM).
set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_ospi_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
