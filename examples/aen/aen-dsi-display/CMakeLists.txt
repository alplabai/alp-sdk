# SPDX-License-Identifier: Apache-2.0
#
# aen-dsi-display -- drive the RK055HDMIPI4MA0 (Rocktech 720x1280, Himax HX8394
# controller, MIPI-DSI) panel through the full Alif Ensemble E8 C2-MIPI-DSI
# DISPLAY chain on an E1M-AEN SoM (M55-HE), via the bench RAM-run + RAM-console
# flow.  This is the pixels-on-glass successor to the BIND-only
# aen-dsi-regcheck.
#
# THE CHAIN (top -> glass):
#   display_write() -> cdc200 (tes,cdc-2.1, DPI pixel pump, framebuffer in SRAM0)
#                   -> mipi_dsi (snps,designware-dsi, DSI-TX host)
#                   -> dphy (snps,designware-dphy, shared D-PHY, TX role)
#                   -> hx8394 panel (himax,hx8394) over 2 DSI data lanes
#
# The whole chain comes from the e1m_evk_rk055hdmipi4ma0 shield
# (zephyr/boards/shields/): it turns on the SoC display nodes, adds the panel,
# the carrier's I2C GPIO expander (panel power + reset) and the framebuffer
# memory region (the top 3 MiB of SRAM0), and enables the drivers.  Any app
# gets the same panel by adding the shield; this one appends it here so a plain
# `west build` of the example needs no -DSHIELD.

cmake_minimum_required(VERSION 3.20)

list(APPEND SHIELD e1m_evk_rk055hdmipi4ma0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_dsi_display LANGUAGES C)

target_sources(app PRIVATE src/main.c)
