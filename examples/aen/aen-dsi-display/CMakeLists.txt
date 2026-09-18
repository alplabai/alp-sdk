# SPDX-License-Identifier: Apache-2.0
#
# aen-dsi-display -- drive the RK055HDMIPI4MA0 (Rocktech 720x1280, Himax HX8394
# controller, MIPI-DSI) panel through the full Alif Ensemble E8 C2-MIPI-DSI
# DISPLAY chain on the E1M-AEN801 (M55-HE), via the bench RAM-run + RAM-console
# flow.  This is the pixels-on-glass successor to the BIND-only aen-dsi-regcheck:
# it turns ON the cdc200 + dsi_dw display-class drivers and adds the panel +
# the I2C GPIO-expander that gates panel reset.
#
# THE CHAIN (top -> glass):
#   display_write() -> cdc200 (tes,cdc-2.1, DPI pixel pump, framebuffer in SRAM0)
#                   -> mipi_dsi (snps,designware-dsi, DSI-TX host)
#                   -> dphy (snps,designware-dphy, shared D-PHY, TX role)
#                   -> hx8394 panel (himax,hx8394) over 2 DSI data lanes
#   panel reset: I2C GPIO expander on I2C2, consumed by the hx8394 reset-gpios
#                path.
#
# FRAMEBUFFER PLACEMENT (RAM-run critical): a 720x1280 RGB565 L1 framebuffer is
# 1,843,200 B -- it does NOT fit ITCM (256 KB), where the RAM-run links code.
# The overlay gives the cdc200 node a memory-region (the top 2 MiB of SRAM0);
# the driver uses that address directly, with no linker section, and flushes
# the data cache after every framebuffer write.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_dsi_display LANGUAGES C)

target_sources(app PRIVATE src/main.c)
