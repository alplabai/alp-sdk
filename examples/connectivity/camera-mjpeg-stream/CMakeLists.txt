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
project(camera_mjpeg_stream LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    src/mjpeg_http.c
)

# Interim Ethernet PHY bring-up (MDIO refclk mode + PWRDWN/RESET GPIOs) --
# AEN-only, and only needed until board generation grows Ethernet routing
# of its own (no portable alp-sdk PHY surface exists yet).  Kept out of
# the link on every other target, including native_sim.
if(BOARD MATCHES "^alp_e1m_aen")
    target_sources(app PRIVATE src/aen_eth_phy.c)
endif()

# CI selftest -- native_sim only (CONFIG_NET_LOOPBACK gives it 127.0.0.1
# to loop back over); see src/selftest.c's file header.
if(BOARD MATCHES "native_sim")
    target_sources(app PRIVATE src/selftest.c)
endif()
