# SPDX-License-Identifier: Apache-2.0
#
# aen-isp-ov5647-viewfinder -- the portable <alp/camera.h> counterpart to
# examples/aen/aen-isp-capture: a real OV5647 frame through the Alif
# ISP-Pico, AE+AWB on, RGB565 out.  See src/main.c and the boards/ overlays.

cmake_minimum_required(VERSION 3.20)

# Apply the board overlay explicitly -- Zephyr's automatic
# boards/<name>.overlay pickup keys off the FULLY-QUALIFIED board name
# (alp_e1m_aen80{1,3}_m55_he_ae822fa0e5597ls0_rtss_he), not the bare board
# name (see aen-isp-capture/CMakeLists.txt for the same note); a
# plain `west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` would
# otherwise drop the ITCM RAM-run retarget both overlays carry.
if(BOARD MATCHES "^alp_e1m_aen801_m55_he")
    set(DTC_OVERLAY_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay")
elseif(BOARD MATCHES "^alp_e1m_aen803_m55_he")
    set(DTC_OVERLAY_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he.overlay")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_isp_ov5647_viewfinder LANGUAGES C)

target_sources(app PRIVATE src/main.c)
