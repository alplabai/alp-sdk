# SPDX-License-Identifier: Apache-2.0
#
# White-box native_sim coverage for the Alif E8 LPRTC counter -> calendar shim
# (#2930: 32-bit CCVR wrap / rebase race, open re-seed).
#
# Like power_alif_se this app does NOT set CONFIG_ALP_SDK=y: the real
# src/backends/rtc/lprtc_calendar_shim.c is #included by the test source (its
# counter device is a devicetree alias that native_sim does not have), against a
# fake counter, a counting spinlock wrapper and a stand-in device.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_lprtc_calendar_shim)

get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)

target_include_directories(app PRIVATE
    ${ALP_SDK_ROOT}/include            # <alp/...>
    ${ALP_SDK_ROOT}/src/backends/rtc   # rtc_ops.h
)

target_sources(app PRIVATE src/test_lprtc_calendar_shim.c)
