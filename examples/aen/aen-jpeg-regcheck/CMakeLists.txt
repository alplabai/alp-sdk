# SPDX-License-Identifier: Apache-2.0
#
# aen-jpeg-regcheck -- see testcase.yaml for the two scenarios this app
# builds under (native_sim software fallback / AEN801 hardware backend).

cmake_minimum_required(VERSION 3.20)

# Apply the AEN801 board overlay explicitly -- Zephyr's automatic
# boards/<name>.overlay pickup keys off the FULLY-QUALIFIED board name
# (alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he), not the bare board name
# (see aen-isp-regcheck/CMakeLists.txt for the same note); a plain
# `west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` would
# otherwise drop the ITCM RAM-run retarget.  Gated on BOARD so this stays a
# no-op on native_sim -- the overlay's `&jpeg0 { ... }` fragment references a
# node that only exists in the Alif E8 SoC dtsi, so applying it
# unconditionally would break the native_sim devicetree compile.
if(BOARD MATCHES "^alp_e1m_aen801_m55_he")
    set(DTC_OVERLAY_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_jpeg_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
