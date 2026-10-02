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
project(blink LANGUAGES C)

target_sources(app PRIVATE src/main.c)
