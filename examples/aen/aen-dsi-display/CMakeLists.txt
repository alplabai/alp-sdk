# SPDX-License-Identifier: Apache-2.0
#
# aen-dsi-display -- drive the RK055HDMIPI4MA0 (Rocktech 720x1280, Himax HX8394
# controller, MIPI-DSI) panel through the full Alif Ensemble E8 C2-MIPI-DSI
# DISPLAY chain on the E1M-AEN801 (M55-HE), via the bench RAM-run + RAM-console
# flow.  This is the pixels-on-glass successor to the BIND-only aen-dsi-regcheck:
# it turns ON the cdc200 + dsi_dw display-class drivers and adds the panel +
# the TCAL9538 I2C GPIO-expander that gates panel power/reset.
#
# THE CHAIN (top -> glass):
#   display_write() -> cdc200 (tes,cdc-2.1, DPI pixel pump, framebuffer in SRAM0)
#                   -> mipi_dsi (snps,designware-dsi, DSI-TX host)
#                   -> dphy (snps,designware-dphy, shared D-PHY, TX role)
#                   -> hx8394 panel (himax,hx8394) over 2 DSI data lanes
#   panel power/reset: TCAL9538 (ti,tca9538) on I2C0 @0x72 -- P0=LCD_PWR_EN,
#                      P1=LCD_RST.
#
# FRAMEBUFFER PLACEMENT (RAM-run critical): a 720x1280 RGB565 L1 framebuffer is
# 1,843,200 B -- it does NOT fit ITCM (256 KB), where the RAM-run links code.
# The cdc200 driver's default framebuffer path emits a `__alif_sram0_section`
# linker attribute that exists only in the Alif fork's link script (NOT in
# alp-sdk / upstream Zephyr).  We therefore compile the driver TU with
# -DNO_RELOCATE_SRAM0, which selects its fixed-address SRAM0 path: the L1 FB is
# pinned at 0x02000000 (E8 SRAM0, multi-MB), with NO linker section needed.  The
# driver already does sys_cache_data_flush_range() after every framebuffer write,
# so the CDC scanout sees coherent pixels (DCACHE stays on).  See src/main.c for
# the full placement note + the pixel-clock bench-tunable.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_dsi_display LANGUAGES C)

# Select the cdc200 driver's fixed-SRAM0 framebuffer path (FB @0x02000000).
# Scoped narrowly to the driver TU: the macro only gates code inside
# display_cdc200.c, but a project-wide definition is the least-surprising knob
# and harmless elsewhere.  See the FRAMEBUFFER PLACEMENT note above.
zephyr_compile_definitions(NO_RELOCATE_SRAM0)

target_sources(app PRIVATE src/main.c)
