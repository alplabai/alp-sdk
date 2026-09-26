# SPDX-License-Identifier: Apache-2.0
#
# aen-camera-firstlight -- first-light bench proof for RPi-style CSI-2
# camera shields on the E1M-AEN801/AEN803 EVK, via the portable
# <alp/camera.h> API. See testcase.yaml for the four camera-shield
# scenarios and README.md for the build command / expected results.

cmake_minimum_required(VERSION 3.20)

# Apply the board overlay explicitly -- Zephyr's automatic
# boards/<name>.overlay pickup keys off the FULLY-QUALIFIED board name
# (alp_e1m_aen80{1,3}_m55_he_ae822fa0e5597ls0_rtss_he), not the bare board
# name (see aen-jpeg-regcheck/CMakeLists.txt for the same note); a plain
# `west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` would
# otherwise drop the ITCM RAM-run retarget both overlays carry.
if(BOARD MATCHES "^alp_e1m_aen801_m55_he")
    set(DTC_OVERLAY_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay")
elseif(BOARD MATCHES "^alp_e1m_aen803_m55_he")
    set(DTC_OVERLAY_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he.overlay")
endif()

# Opt-in external-trigger demo (issue #2287): IMX296's fast-trigger exposure
# mode plus a GPIO pulse on the sensor module's J3 Trig+ (see
# trigger_gpio.overlay for the pin chain and its own header comment). OFF by
# default -- a normal firstlight run stays exactly the free-run capture it
# always was, on every shield, not just IMX296's. Only meaningful with
# -DSHIELD="e1m_evk_rpi_csi raspberry_pi_global_shutter_camera"; harmless
# (compiles, does nothing at runtime) on the other two shields since
# CONFIG_VIDEO_IMX296 gates the trigger code path in main.c too.
#
# Deliberately NOT under boards/: it is force-included here via
# DTC_OVERLAY_FILE regardless of BOARD, not a per-board overlay Zephyr's
# board-name auto-apply would pick up -- check_example_portability.py's
# check_dead_board_overlays() requires every boards/*.overlay filename to
# name a real board, which this isn't (it's named for the CMake option that
# gates it). Living at the app root instead of boards/ keeps that gate
# scoped to what it actually checks.
#   -DAEN_CAMERA_TRIGGER=ON
option(AEN_CAMERA_TRIGGER "Drive IMX296 in fast-trigger mode via a GPIO pulse" OFF)
if(AEN_CAMERA_TRIGGER)
    list(APPEND DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/trigger_gpio.overlay")
endif()

# Opt-in IMX296 ROI-mode demo (issue #2287 Stage B): selects the sensor's second, centred
# 1280x960 crop (zephyr/drivers/video/imx296.c's IMX296_ROI_WIDTH/HEIGHT) instead of the default
# full 1456x1088 frame -- see src/main.c's CAM_WIDTH/CAM_HEIGHT #elif ladder, which is the only
# other place this define is read. OFF by default: a normal IMX296 build stays the more
# bench-tested full-frame capture it always was. No DT overlay needed (unlike
# AEN_CAMERA_TRIGGER above) -- the ROI registers are driven entirely by imx296_set_stream()
# reading back the format video_set_format() selected, not by anything in devicetree.
#   -DAEN_CAMERA_IMX296_ROI=ON
option(AEN_CAMERA_IMX296_ROI "Capture IMX296's 1280x960 ROI crop instead of the full frame" OFF)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_camera_firstlight LANGUAGES C)

target_sources(app PRIVATE src/main.c)

if(AEN_CAMERA_TRIGGER)
    target_compile_definitions(app PRIVATE AEN_CAMERA_TRIGGER=1)
endif()

if(AEN_CAMERA_IMX296_ROI)
    target_compile_definitions(app PRIVATE AEN_CAMERA_IMX296_ROI=1)
endif()
