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

# Opt-in IMX296 variant (issue #2287 Stage B): -DAEN_ISP_IMX296=ON selects IMX296's ROI
# (1280x960 SRGGB10P) input instead of OV5647's native 640x480 SBGGR10P -- see src/main.c's
# FRAME_WIDTH/HEIGHT/N_FRAMES/ISP_INPUT_FOURCC #if ladder, which is the only other place this
# define is read. OFF by default: a normal build stays the OV5647 path this example was proven
# against. Needs the raspberry_pi_global_shutter_camera shield in place of
# raspberry_pi_camera_module_1 (see testcase.yaml's imx296 scenario) and hal_alif patch 0012
# (zephyr/patches/hal_alif/0012-isp-srggb10p-input.patch) for the ISP wrapper's SRGGB10P mapping.
#   -DAEN_ISP_IMX296=ON
option(AEN_ISP_IMX296 "Run IMX296's ROI crop through the ISP instead of OV5647" OFF)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_isp_ov5647_capture LANGUAGES C)

target_sources(app PRIVATE src/main.c)

if(AEN_ISP_IMX296)
    target_compile_definitions(app PRIVATE AEN_ISP_IMX296=1)
endif()
