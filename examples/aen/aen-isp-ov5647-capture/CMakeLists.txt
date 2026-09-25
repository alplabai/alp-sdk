# SPDX-License-Identifier: Apache-2.0
#
# aen-isp-ov5647-capture -- a real OV5647 sensor frame through the ISP
# (sensor -> csi -> cam -> isp -> memory), mirroring Alif's own sdk-alif
# samples/drivers/viewfinder recipe, with AE + AWB running through
# isp_pico.c's standard Zephyr video ctrl path. Build with the SAME two
# shields aen-camera-firstlight's OV5647 scenario uses:
#   -DSHIELD="e1m_evk_rpi_csi raspberry_pi_camera_module_1"
# See testcase.yaml for the exact scenario and README.md (if present) for the
# build command / expected results.

cmake_minimum_required(VERSION 3.20)

# Apply the board overlay explicitly -- Zephyr's automatic
# boards/<name>.overlay pickup keys off the FULLY-QUALIFIED board name
# (alp_e1m_aen80{1,3}_m55_he_ae822fa0e5597ls0_rtss_he), not the bare board
# name (see aen-camera-firstlight/CMakeLists.txt for the same note); a plain
# `west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` would
# otherwise drop the ITCM RAM-run retarget both overlays carry.
if(BOARD MATCHES "^alp_e1m_aen801_m55_he")
    set(DTC_OVERLAY_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay")
elseif(BOARD MATCHES "^alp_e1m_aen803_m55_he")
    set(DTC_OVERLAY_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he.overlay")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_isp_ov5647_capture LANGUAGES C)

target_sources(app PRIVATE src/main.c)
