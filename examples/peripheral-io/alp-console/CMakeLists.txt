# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

# SoM facts follow the board being built, not board.yaml's static som.sku
# (#2597): gen_example_alp_conf.py writes generated/<sku>/alp.conf for a twin
# SKU whose fragment differs (e.g. AEN803's populated OSPI memories).
if(BOARD MATCHES "^alp_e1m_(aen[0-9]+)"
   AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/generated/${CMAKE_MATCH_1}/alp.conf")
    string(REPLACE "generated/alp.conf" "generated/${CMAKE_MATCH_1}/alp.conf"
           EXTRA_CONF_FILE "${EXTRA_CONF_FILE}")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_console LANGUAGES C)

target_sources(app PRIVATE src/main.c)

# Reusable SoM bring-up helper (cc3501e_bridge_bringup): the one-call
# CC3501E bridge init.  Copied verbatim from
# examples/aen/aen-cc3501e-bringup/src/ so this example is self-contained.
# When CONFIG_ALP_SDK_CHIP_CC3501E is off (native_sim / V2N) the TU still
# compiles -- it just resolves to the stub cc3501e_bridge_bringup() that
# returns ALP_ERR_NOT_PRESENT_ON_THIS_SOC immediately.
if(CONFIG_ALP_SDK_CHIP_CC3501E)
    target_sources(app PRIVATE src/cc3501e_bridge.c)
endif()
