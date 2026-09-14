# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_sdhc_probe LANGUAGES C)
target_sources(app PRIVATE src/main.c)

# BENCH-TEST (test/2051-sdhc-enable-on-reworked-mux), not upstream: the
# CC3501E bridge bring-up helper + this app's one-entry GPIO-proxy route
# table, needed to drive the SDIO mux ENABLE (E1M IO20) before touching
# sdhc0 on the reworked e1m-aen-evk-03 bench board -- see the board overlay
# and src/main.c. Only linked in when the board overlay enables sdhc0 (the
# reworked-mux configuration); a build where sdhc0 stays disabled has no use
# for the bridge, but compiling these unconditionally costs nothing (their
# own #if DT_NODE_HAS_STATUS guards live in main.c's call sites, not here).
target_sources(app PRIVATE src/cc3501e_bridge.c src/cc3501e_gpio_routes.c)
