# SPDX-License-Identifier: Apache-2.0
#
# aen-spi-regcheck -- scopeless on-silicon validation of the Alif DWC_ssi SPI
# driver (zephyr/drivers/spi/spi_dw_alif.c, compatible "alif,dwc-ssi-spi") on the
# E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run + RAM-console flow.
#
# Plain prj.conf app (no alp_project.py board.yaml emit): the driver Kconfig
# (CONFIG_SPI_DW_ALIF, default y, depends on DT_HAS_ALIF_DWC_SSI_SPI_ENABLED) is
# sourced under CONFIG_ALP_SDK, which prj.conf sets.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_spi_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
