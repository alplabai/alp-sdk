# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_spi_dw_alif_dma_burst)

# The logic under test is the dependency-free static-inline
# spi_dw_dma_calculate_burst_length() / spi_dw_dma_burst_for_chunk() in
# zephyr/drivers/spi/spi_dw_alif_dma_burst.h -- the same header
# spi_dw_alif.c's spi_dw_dma_setup_tx_channel() / spi_dw_dma_setup_rx_channel()
# call -- so the test needs only <stdbool.h>/<stddef.h>/<stdint.h>, no
# DEVICE_MMIO or a devicetree-instantiated SPI instance.
#
# KNOWN GAP, same shape as tests/unit/dma_pl330_sg_validate's: this proves the
# header functions themselves are correct, not that spi_dw_alif.c still calls
# them for the dma_cfg burst length on both directions (and for DMARDLR on
# RX -- TX's DMATDLR is deliberately NOT this value; see
# spi_dw_alif_dma_burst.h) -- a mutation reverting a call site is invisible
# here. spi_dw_alif.c is not buildable under native_sim in this environment
# (vendored Alif DWC_ssi register layer, hal_alif-adjacent), so closing that
# gap needs an AEN-target twister build, which is not part of any CI gate
# today (pr-twister-aen.yml build-only scenarios don't run unit-style native
# tests). Left open; call this out explicitly rather than claim it's covered.
target_sources(app PRIVATE src/test_spi_dw_alif_dma_burst.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/spi)
