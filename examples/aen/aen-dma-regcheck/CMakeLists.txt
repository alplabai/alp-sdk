# SPDX-License-Identifier: Apache-2.0
#
# aen-dma-regcheck -- on-silicon validation of the ARM PL330 DMA controller on
# the E1M-AEN801 (Alif Ensemble E8, M55-HE), via the bench RAM-run + RAM-console
# flow.  Unlike the bind-only *-regcheck siblings, this app runs a REAL
# memory-to-memory copy through the PL330 and verifies the bytes (the PL330 needs
# no external pins).
#
# The PL330 driver is UPSTREAM Zephyr v4.4's drivers/dma/dma_pl330.c
# (compatible "arm,dma-pl330") -- ADR 0017 Tier-1 (upstream-native), no vendored
# code.  CONFIG_DMA + CONFIG_DMA_PL330 (prj.conf) build it in once the dma2 node
# is enabled by the board overlay.  The overlay enables dma2, defines the
# alp_dma0 alias, points the PL330 `microcode` scratch at global SRAM0, and
# carves the source/destination buffers into SRAM0 (the PL330 AXI master cannot
# reach the M55's DTCM).
#
# The board overlay boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay auto-applies for the
# fully-qualified board (alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he).  Build:
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_dma_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
