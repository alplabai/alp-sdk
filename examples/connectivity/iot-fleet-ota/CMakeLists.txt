# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(iot_fleet_ota LANGUAGES C)

target_sources(app PRIVATE src/main.c)
# Reusable SoM bring-up helper (cc3501e_bridge_bringup): the one-call CC3501E
# bridge init this app uses so alp_wifi_open() can bind to a ready bridge on
# E1M-AEN801 (#2112). Copyable template, same file as examples/aen/aen-cc3501e-*
# -- see src/cc3501e_bridge.h. Built unconditionally; alp_gpio_open()/alp_spi_open()
# fail fast (NULL, no matching backend) on native_sim, so this is a no-op there.
target_sources(app PRIVATE src/cc3501e_bridge.c)
