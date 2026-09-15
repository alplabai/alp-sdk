# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_i2s_tas2563_probe LANGUAGES C)
target_sources(app PRIVATE src/main.c)

# BENCH-TEST (test/u46-i2s-tas2563-on-reworked-mux), not upstream: the
# CC3501E bridge bring-up helper + this app's two-entry GPIO-proxy route
# table, needed to drive the I2S0 mux SELECT + ENABLE (E1M IO13/IO8) before
# touching i2s3 on the reworked e1m-aen-evk-03 bench board -- see the board
# overlay and src/main.c.
target_sources(app PRIVATE src/cc3501e_bridge.c src/cc3501e_gpio_routes.c)
