# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_dma_pl330_sg_validate)

# The guard under test is the dependency-free static-inline
# dma_pl330_sg_chain_matches() in zephyr/drivers/dma/dma_pl330_sg_validate.h
# -- the same header dma_pl330_alif.c's dma_pl330_configure() calls -- so the
# test needs only <zephyr/drivers/dma.h>'s struct dma_block_config, no
# DEVICE_MMIO or a devicetree-instantiated DMA instance.
target_sources(app PRIVATE src/test_dma_pl330_sg_validate.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/dma)
