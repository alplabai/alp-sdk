# SPDX-License-Identifier: Apache-2.0
#
# aen-lvds-display -- drive the Riverdi RVT121HVDFWCA0-B (12.1" 1280x800 LVDS)
# panel through the Alif Ensemble E8 MIPI-DSI chain PLUS a TI SN65DSI83
# DSI-to-LVDS bridge on a maintainer-built adapter PCB, on an E1M-AEN SoM
# (M55-HE), via the bench RAM-run + RAM-console flow.  A separate example from
# aen-dsi-display (RK055/hx8394, a native DSI panel) rather than an extension
# of it: this panel is not a DSI peripheral at all -- there is no panel@N DT
# node and no DCS traffic -- so the two examples share almost no code once you
# get past "fill a framebuffer and call display_write()".
#
# THE CHAIN (top -> glass):
#   display_write() -> cdc200 (tes,cdc-2.1, DPI pixel pump, framebuffer in SRAM0)
#                   -> mipi_dsi (snps,designware-dsi, DSI-TX host)
#                   -> dphy (snps,designware-dphy, shared D-PHY, TX role)
#                   -> bridge@2c (ti,sn65dsi83, I2C1, on the adapter PCB)
#                   -> Riverdi RVT121HVDFWCA0-B panel (off-shield, LVDS)
#
# The whole chain comes from the e1m_evk_rvt121hvdfwca0 shield
# (zephyr/boards/shields/): it turns on the SoC display nodes, adds the
# bridge, the carrier's I2C GPIO expander (bridge EN) and the framebuffer
# memory region, and enables the drivers.  This example appends the shield
# here so a plain `west build` needs no -DSHIELD.

cmake_minimum_required(VERSION 3.20)

list(APPEND SHIELD e1m_evk_rvt121hvdfwca0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_lvds_display LANGUAGES C)

target_sources(app PRIVATE src/main.c)
