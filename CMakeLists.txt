# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alp Lab AB
#
# Alp SDK top-level CMakeLists.
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
    DESCRIPTION "Unification SDK for Alp Lab E1M edge AI modules"
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

option(ALP_BUILD_TESTS    "Build Alp SDK tests"    OFF)
option(ALP_BUILD_BENCH    "Build Alp SDK microbenchmarks (tests/bench/)"  OFF)
option(ALP_BUILD_FUZZ     "Build Alp SDK libFuzzer harnesses (tests/fuzz/, clang-only)"  OFF)
option(ALP_HAS_LVGL       "Compile <alp/gui.h> against LVGL"  OFF)
option(ALP_HAS_CMSIS_DSP  "Allow Alp SDK internals (audio etc.) to call CMSIS-DSP.  Application code uses CMSIS-DSP directly; ALP no longer re-exports it." OFF)

# Build libalp_sdk as a shared library and install it + the <alp/*>
# headers.  Linux/Yocto consumers (e.g. the meta-alp-sdk recipe, ROS 2
# nodes that link libalp_sdk.so) set this ON; embedded users who
# add_subdirectory() the SDK leave it OFF and get the static,
# install-free build they expect.
option(ALP_SDK_BUILD_SHARED "Build libalp_sdk.so + install lib/headers (Linux/Yocto)"  OFF)

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
# Chip-driver collection (libalp_chips)
#
# chips/<part>/<part>.c are OS-agnostic C drivers that call back into
# <alp/peripheral.h> (alp_i2c_* / alp_spi_* / ...), resolved at final
# link against libalp_sdk.  Each driver is opt-in via a per-chip
# ALP_SDK_CHIP_<NAME> toggle (default OFF) so a consumer builds exactly
# the drivers its board populates.  The Yocto `alp-chips` recipe drives
# this with ALP_BUILD_CHIPS_ONLY=ON + the toggles, emitting a static
# libalp_chips.a that apps (e.g. the alp_perception ROS node) link
# alongside libalp_sdk.so.
# ---------------------------------------------------------------------
set(ALP_SDK_CHIP_LIST
    tcal9538 ina236 tas2563 lsm6dso bmi323 bmp581 icm42670
    ssd1306 ssd1331 tmp112 rv3028c7 optiga_trust_m eeprom_24c128
    ov5640 cam_mux_pi3wvr626 ublox_neo_m9n
    CACHE INTERNAL "Chip drivers selectable via ALP_SDK_CHIP_<NAME>")
option(ALP_BUILD_CHIPS_ONLY
    "Build the libalp_chips static archive from the enabled chip drivers (Yocto alp-chips recipe)." OFF)
foreach(_chip IN LISTS ALP_SDK_CHIP_LIST)
    string(TOUPPER "${_chip}" _chip_uc)
    option(ALP_SDK_CHIP_${_chip_uc} "Build the ${_chip} driver into libalp_chips" OFF)
endforeach()

if(ALP_BUILD_CHIPS_ONLY)
    add_library(alp_chips STATIC)
    target_link_libraries(alp_chips PUBLIC alp_sdk_headers)
    set(_alp_chips_enabled "")
    foreach(_chip IN LISTS ALP_SDK_CHIP_LIST)
        string(TOUPPER "${_chip}" _chip_uc)
        if(ALP_SDK_CHIP_${_chip_uc})
            target_sources(alp_chips PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/chips/${_chip}/${_chip}.c)
            list(APPEND _alp_chips_enabled ${_chip})
        endif()
    endforeach()
    message(STATUS "alp-sdk: libalp_chips drivers: ${_alp_chips_enabled}")
    include(GNUInstallDirs)
    install(TARGETS alp_chips ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/alp/chips
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/alp)
endif()

# ---------------------------------------------------------------------
# Implementation library
#
# In Zephyr-module mode the per-backend CMakeLists uses zephyr_library_*
# helpers, so we skip the raw STATIC library here.  Plain-CMake mode
# emits alp_sdk + alp::sdk as the consumer target.
# ---------------------------------------------------------------------

if(NOT DEFINED ZEPHYR_BASE)
    if(ALP_SDK_BUILD_SHARED)
        add_library(alp_sdk SHARED)
        set_target_properties(alp_sdk PROPERTIES
            VERSION   ${PROJECT_VERSION}
            SOVERSION ${PROJECT_VERSION_MAJOR})
    else()
        add_library(alp_sdk STATIC)
    endif()
    target_link_libraries(alp_sdk PUBLIC alp_sdk_headers)
    add_library(alp::sdk ALIAS alp_sdk)
    # Plain-CMake builds link libalp_sdk as an ordinary (static or shared)
    # library, NOT with Zephyr's whole-archive model.  A static-archive
    # link only pulls the backend-registry members a dispatcher actually
    # references, which drops every backend that lives purely in an
    # alp_backends_<class> section and leaves the section-bound
    # __start_/__stop_ symbols undefined (issue #368).  This switch turns
    # on the ALP_BACKEND_ANCHOR macros (see <alp/backend.h>) so each
    # dispatcher force-references its section-carrying backend and pulls
    # it into the link.  Harmless (inert) for the shared build.
    target_compile_definitions(alp_sdk PRIVATE ALP_BACKEND_STATIC_ANCHORS=1)
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

if(ALP_BUILD_BENCH)
    add_subdirectory(tests/bench)
endif()

if(ALP_BUILD_FUZZ)
    add_subdirectory(tests/fuzz)
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

# Install rules.  Only emitted for the shared/standalone build
# (ALP_SDK_BUILD_SHARED) so the add_subdirectory() flow embedded
# consumers use is unchanged -- they never invoke `install`.  This is
# what lets the meta-alp-sdk Yocto recipe package libalp_sdk.so + the
# <alp/*> headers into the target sysroot (so e.g. the alp_perception
# ROS node's find_library(alp_sdk) / find_path(alp/peripheral.h)
# resolve).
if(ALP_SDK_BUILD_SHARED AND NOT DEFINED ZEPHYR_BASE)
    include(GNUInstallDirs)
    install(TARGETS alp_sdk
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    # alp/chips/ headers are owned by the alp-chips package; exclude them
    # so a sysroot with both alp-sdk and alp-chips has no file collision.
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/alp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        PATTERN "chips" EXCLUDE)
endif()
