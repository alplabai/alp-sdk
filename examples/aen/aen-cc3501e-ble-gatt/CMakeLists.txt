# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_cc3501e_ble_gatt LANGUAGES C)

target_sources(app PRIVATE src/main.c)
# Reusable SoM bring-up helper (cc3501e_bridge_bringup): the one-call CC3501E
# bridge init the example + applications use.  Copied VERBATIM from
# aen-cc3501e-companion-tour -- see cc3501e_bridge.h for why it's a
# copyable template.
target_sources(app PRIVATE src/cc3501e_bridge.c)
