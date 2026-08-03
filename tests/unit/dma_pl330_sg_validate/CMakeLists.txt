# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_dma_pl330_sg_validate)

# The guard under test is the dependency-free static-inline
# dma_pl330_sg_chain_matches() in zephyr/drivers/dma/dma_pl330_sg_validate.h
# -- the same header dma_pl330_alif.c's dma_pl330_configure() calls -- so the
# test needs only <zephyr/drivers/dma.h>'s struct dma_block_config, no
# DEVICE_MMIO or a devicetree-instantiated DMA instance.
#
# KNOWN GAP (round-2 #1124 dev-review, not closed by this suite): this only
# proves the header function itself is correct, not that dma_pl330_alif.c's
# dma_pl330_configure() still calls it -- a mutation deleting that one call
# (proven in review: `if (false)` in its place, 3/3 configurations still
# passed) is invisible here. Unlike display_cdc200.c (see
# tests/unit/display_cdc200_entrypoints), dma_pl330_alif.c unconditionally
# `#include <soc.h>` and `#include <soc_memory_map.h>` (Alif hal_alif-module
# headers for `local_to_global()`) and is not buildable under native_sim in
# this environment -- hal_alif is a west module not present here (see memory
# reference_hal_alif_module_and_video_port). Closing this gap needs either
# vendoring a minimal `local_to_global()`/`soc_memory_map.h` test stub (risk:
# masks a real hal_alif integration break) or an AEN-target twister build,
# which is not part of any CI gate today (pr-twister-aen.yml build-only
# scenarios don't run unit-style native tests). Left open; call this out
# explicitly rather than claim it's covered.
target_sources(app PRIVATE src/test_dma_pl330_sg_validate.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/dma)
