# SPDX-License-Identifier: Apache-2.0
#
# Vendor wrapper: turns TI's (license-gated, NOT redistributed) SimpleLink
# Wi-Fi SDK for the CC35xxE family into the `simplelink_cc33xx` CMake target
# that the cc3501e-bridge `ti` build links against
# (firmware/cc3501e/CMakeLists.txt: target_link_libraries(... simplelink_cc33xx)).
#
# alp-sdk does NOT ship the SDK.  Obtain it from TI (see README.md), then
# point this wrapper at the extracted SDK via EITHER:
#   * -DSIMPLELINK_SDK_DIR=<path-to-extracted-sdk>            (external; recommended)
#   * copy/symlink it into  ./sdk/  (gitignored)              (in-tree)
#
# Built ONLY when CC3501E_HAL_BACKEND=ti (the bench build); the stub/CI path
# never descends here.

cmake_minimum_required(VERSION 3.20)

# --- Locate the extracted SDK ------------------------------------------------
if(NOT DEFINED SIMPLELINK_SDK_DIR)
    set(SIMPLELINK_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/sdk")
endif()

if(NOT EXISTS "${SIMPLELINK_SDK_DIR}")
    message(FATAL_ERROR
        "SimpleLink CC33xx SDK not found at '${SIMPLELINK_SDK_DIR}'.\n"
        "alp-sdk does not redistribute it -- see "
        "firmware/cc3501e/vendor/simplelink-cc33xx/README.md to obtain it from TI, "
        "then either populate ./sdk/ or pass -DSIMPLELINK_SDK_DIR=<path>.")
endif()

# --- SDK source / include groups (FILL on the bench) -------------------------
# These are SDK-version-specific -- do NOT guess them.  Fill from the EXTRACTED
# SDK layout once it is in hand (record the pinned version in README.md).  The
# `ti` HAL (../../hal/ti/*) needs, at minimum:
#   * TI Drivers        : SPI, GPIO              (ti/drivers/SPI.h, GPIO.h)
#   * SimpleLink host    : sl_Start / sl_NetCfgGet (ti/drivers/net/wifi/*)
#   * SysConfig output   : ti_drivers_config.{c,h} for the E1M-AEN board
#                          (CONFIG_SPI_0 -> GPIO_27/28/29; see README.md)
#   * RTOS + stacks (v0.2+): FreeRTOS, LwIP, the BLE 5.4 host
set(SIMPLELINK_CC33XX_SOURCES  "" CACHE STRING "SDK + SysConfig .c sources for the bridge")
set(SIMPLELINK_CC33XX_INCLUDES "" CACHE STRING "SDK include directories")

if(SIMPLELINK_CC33XX_SOURCES STREQUAL "")
    message(FATAL_ERROR
        "simplelink_cc33xx is a SCAFFOLD: set SIMPLELINK_CC33XX_SOURCES and "
        "SIMPLELINK_CC33XX_INCLUDES from the extracted SDK layout (see the "
        "group list above + README.md).  This is filled once on the bench "
        "against the pinned SDK version, then committed.")
endif()

add_library(simplelink_cc33xx STATIC ${SIMPLELINK_CC33XX_SOURCES})
target_include_directories(simplelink_cc33xx PUBLIC ${SIMPLELINK_CC33XX_INCLUDES})

# The CC3501E application core is a Cortex-M33; keep the ABI consistent with
# the cc3501e-bridge target's flags (see firmware/cc3501e/CMakeLists.txt).
target_compile_options(simplelink_cc33xx PRIVATE -mcpu=cortex-m33 -mthumb)
