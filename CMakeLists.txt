# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 ALP Lab AB
#
# ALP SDK top-level CMakeLists.
#
# Two consumption modes are supported:
#
# 1. Zephyr module — when ZEPHYR_BASE is defined, this file is included
#    by the Zephyr build via zephyr/module.yml.  In that mode we
#    register an interface library (alp_sdk) against the Zephyr
#    target and let zephyr/CMakeLists.txt drive source selection.
#
# 2. Plain CMake `add_subdirectory()` — for bare-metal and Yocto
#    consumers that don't use west.  ALP_OS picks the backend dir.
#
# Either way the public include surface is the same: <alp/...>.

cmake_minimum_required(VERSION 3.20)

project(alp-sdk
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Unification SDK for ALP Lab E1M edge AI modules"
    HOMEPAGE_URL "https://github.com/alplabai/alp-sdk"
)

# ---------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------

set(ALP_SUPPORTED_OS "zephyr;baremetal;yocto" CACHE INTERNAL "")

if(DEFINED ZEPHYR_BASE)
    set(_alp_default_os "zephyr")
else()
    set(_alp_default_os "baremetal")
endif()

set(ALP_OS "${_alp_default_os}" CACHE STRING
    "Target OS backend: zephyr, baremetal, or yocto")
set_property(CACHE ALP_OS PROPERTY STRINGS ${ALP_SUPPORTED_OS})

if(NOT ALP_OS IN_LIST ALP_SUPPORTED_OS)
    message(FATAL_ERROR
        "ALP_OS='${ALP_OS}' is not one of ${ALP_SUPPORTED_OS}")
endif()

# SoM family selector -- picks which vendor wrapper layer the
# bare-metal backend pulls in.  none = peripheral stubs only;
# aen = vendors/alif/ wrappers (Alif Ensemble); v2n / imx93
# slot in once their respective vendor packs land.
set(ALP_SUPPORTED_SOM "none;aen;v2n;imx93" CACHE INTERNAL "")
set(ALP_SOM "none" CACHE STRING
    "Target SoM family for bare-metal: none, aen, v2n, imx93")
set_property(CACHE ALP_SOM PROPERTY STRINGS ${ALP_SUPPORTED_SOM})
if(NOT ALP_SOM IN_LIST ALP_SUPPORTED_SOM)
    message(FATAL_ERROR
        "ALP_SOM='${ALP_SOM}' is not one of ${ALP_SUPPORTED_SOM}")
endif()

option(ALP_BUILD_TESTS    "Build ALP SDK tests"    OFF)
option(ALP_HAS_LVGL       "Compile <alp/gui.h> against LVGL"  OFF)
option(ALP_HAS_CMSIS_DSP  "Allow ALP SDK internals (audio etc.) to call CMSIS-DSP.  Application code uses CMSIS-DSP directly; ALP no longer re-exports it." OFF)

# ---------------------------------------------------------------------
# Public header library
# ---------------------------------------------------------------------

add_library(alp_sdk_headers INTERFACE)
target_include_directories(alp_sdk_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(alp_sdk_headers INTERFACE c_std_99)

if(ALP_HAS_LVGL)
    target_compile_definitions(alp_sdk_headers INTERFACE ALP_HAS_LVGL=1)
endif()
if(ALP_HAS_CMSIS_DSP)
    target_compile_definitions(alp_sdk_headers INTERFACE ALP_HAS_CMSIS_DSP=1)
endif()

# Convenience alias for downstream users.
add_library(alp::sdk_headers ALIAS alp_sdk_headers)

# ---------------------------------------------------------------------
# Implementation library
#
# In Zephyr-module mode the per-backend CMakeLists uses zephyr_library_*
# helpers, so we skip the raw STATIC library here.  Plain-CMake mode
# emits alp_sdk + alp::sdk as the consumer target.
# ---------------------------------------------------------------------

if(NOT DEFINED ZEPHYR_BASE)
    add_library(alp_sdk STATIC)
    target_link_libraries(alp_sdk PUBLIC alp_sdk_headers)
    add_library(alp::sdk ALIAS alp_sdk)
endif()

# Common helpers (always built).
add_subdirectory(src/common)

# OS backend.
add_subdirectory(src/${ALP_OS})

# Vendor wrappers — added on demand by the OS backend's CMakeLists.

# ---------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------

if(ALP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------
# find_package() support
# ---------------------------------------------------------------------

include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/AlpSdkConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/AlpSdkConfig.cmake
    INSTALL_DESTINATION lib/cmake/AlpSdk
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/AlpSdkConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# Install rules are intentionally minimal in v0.1.  Embedded consumers
# typically use add_subdirectory() and don't need installed artifacts.
