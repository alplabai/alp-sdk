# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

# native_sim can't build board.yaml's `diagnostics.console: ram` (see
# boards/native_sim_console.conf) -- append the override AFTER the
# generated alp.conf so it wins the merge, instead of relying on Zephyr's own
# `boards/<board>.conf` auto-discovery (which merges BEFORE alp.conf and would
# lose).
if(BOARD MATCHES "^native_sim")
    list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/boards/native_sim_console.conf)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_cc3501e_gatt_register LANGUAGES C)

target_sources(app PRIVATE src/main.c)
# Reusable SoM bring-up helper (cc3501e_bridge_bringup): the one-call CC3501E
# bridge init the example + applications use. Copyable template -- see
# cc3501e_bridge.h. (Same helper as aen-cc3501e-companion-tour; examples are
# meant to be self-contained, copyable starting points.)
target_sources(app PRIVATE src/cc3501e_bridge.c)
