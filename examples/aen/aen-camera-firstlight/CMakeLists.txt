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
# mode plus a GPIO pulse on the sensor module's J3 Trig+ (see boards/trigger_gpio.overlay
# for the pin chain and its own header comment). OFF by default -- a normal
# firstlight run stays exactly the free-run capture it always was, on every
# shield, not just IMX296's. Only meaningful with
# -DSHIELD="e1m_evk_rpi_csi raspberry_pi_global_shutter_camera"; harmless
# (compiles, does nothing at runtime) on the other two shields since
# CONFIG_VIDEO_IMX296 gates the trigger code path in main.c too.
#   -DAEN_CAMERA_TRIGGER=ON
option(AEN_CAMERA_TRIGGER "Drive IMX296 in fast-trigger mode via a GPIO pulse" OFF)
if(AEN_CAMERA_TRIGGER)
    list(APPEND DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/trigger_gpio.overlay")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_camera_firstlight LANGUAGES C)

target_sources(app PRIVATE src/main.c)

if(AEN_CAMERA_TRIGGER)
    target_compile_definitions(app PRIVATE AEN_CAMERA_TRIGGER=1)
endif()
