# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_sdcard_readout LANGUAGES C)
target_sources(app PRIVATE src/main.c)
# The CC3501E bridge bring-up + the SDIO mux ENABLE it drives: on the EVK the
# microSD sits behind a 74LVC157 mux whose /E is CC3501E-side, so the card is
# electrically disconnected unless this app brings the bridge up itself.
# cc3501e_bridge.{c,h} is the SoM bring-up TEMPLATE every AEN application
# copies verbatim (see its header) -- byte-identical to the copy in
# examples/aen/aen-evk-demo, the silicon-proven origin of this one; copied
# rather than reached across into, because an example that reads its sources
# out of a sibling example is not a copyable template.
target_sources(app PRIVATE src/cc3501e_bridge.c)
# The one proxied pad this app drives: E1M IO20 -> raw CC3501E GPIO_26, the
# SDIO mux ENABLE route. Strong override of the WEAK cc3501e_gpio_routes[] in
# src/backends/gpio/cc3501e_proxy_routes_weak.c. Built unconditionally; only
# consulted when CONFIG_ALP_SDK_GPIO_CC3501E_PROXY is set (prj.conf).
target_sources(app PRIVATE src/cc3501e_gpio_routes.c)
