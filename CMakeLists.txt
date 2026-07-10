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

# ---------------------------------------------------------------------
# Version -- metadata/sdk_version.yaml is the single source of truth;
# scripts/bump_version.py mirrors it into the ALP_VERSION_MAJOR/MINOR/
# PATCH macros in include/alp/version.h on every release bump
# (scripts/check_version_doc_sync.py keeps the two in lockstep).  Parse
# those macros here instead of the YAML directly so CMake needs no
# YAML parser.  This file previously hardcoded VERSION 0.1.0, which
# never tracked a real release: PROJECT_VERSION (and therefore the
# installed AlpSdkConfigVersion.cmake `find_package(alp-sdk 0.9 ...)`
# resolves against) now always matches ALP_VERSION_STRING (issue #607).
# ---------------------------------------------------------------------
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/alp/version.h" _alp_version_h_lines
    REGEX "^#define[ \t]+ALP_VERSION_(MAJOR|MINOR|PATCH)[ \t]+[0-9]+")
foreach(_alp_version_h_line IN LISTS _alp_version_h_lines)
    if(_alp_version_h_line MATCHES "ALP_VERSION_MAJOR[ \t]+([0-9]+)")
        set(_alp_sdk_version_major "${CMAKE_MATCH_1}")
    elseif(_alp_version_h_line MATCHES "ALP_VERSION_MINOR[ \t]+([0-9]+)")
        set(_alp_sdk_version_minor "${CMAKE_MATCH_1}")
    elseif(_alp_version_h_line MATCHES "ALP_VERSION_PATCH[ \t]+([0-9]+)")
        set(_alp_sdk_version_patch "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(NOT DEFINED _alp_sdk_version_major OR NOT DEFINED _alp_sdk_version_minor
        OR NOT DEFINED _alp_sdk_version_patch)
    message(FATAL_ERROR
        "alp-sdk: could not parse ALP_VERSION_MAJOR/MINOR/PATCH from "
        "include/alp/version.h -- has the macro layout changed?")
endif()
set(_alp_sdk_version
    "${_alp_sdk_version_major}.${_alp_sdk_version_minor}.${_alp_sdk_version_patch}")

project(alp-sdk
    VERSION ${_alp_sdk_version}
    LANGUAGES C CXX
    DESCRIPTION "Unification SDK for Alp Lab E1M edge AI modules"
    HOMEPAGE_URL "https://github.com/alplabai/alp-sdk"
)

unset(_alp_version_h_lines)
unset(_alp_version_h_line)
unset(_alp_sdk_version_major)
unset(_alp_sdk_version_minor)
unset(_alp_sdk_version_patch)
unset(_alp_sdk_version)

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

# Mirrors the Zephyr Kconfig CONFIG_ALP_STATUS_DESCRIPTIONS (zephyr/Kconfig,
# default y) for plain-CMake (baremetal/yocto) consumers: alp_status_name()
# is always compiled, but its sibling alp_status_description()'s string
# table (src/status_strings.c) is gated behind this so a footprint-sensitive
# build can drop it.  ON by default here too, matching the Kconfig default.
option(ALP_SDK_STATUS_DESCRIPTIONS
    "Compile the alp_status_description() string table (src/status_strings.c)"
    ON)

# Build libalp_sdk as a shared library and install it + the <alp/*>
# headers.  Linux/Yocto consumers (e.g. the meta-alp-sdk recipe, ROS 2
# nodes that link libalp_sdk.so) set this ON; embedded users who
# add_subdirectory() the SDK leave it OFF and get the static,
# install-free build they expect.
option(ALP_SDK_BUILD_SHARED "Build libalp_sdk.so + install lib/headers (Linux/Yocto)"  OFF)

# Alp-maintainer strict-warning profile (issue #634) -- defines the
# alp_sdk_warnings INTERFACE target + the ALP_SDK_STRICT_WARNINGS /
# ALP_SDK_WERROR options (both OFF by default) plus the
# alp_sdk_apply_strict_warnings() helper.  Included before any first-party
# target below: alp_chips links alp_sdk_warnings PRIVATE directly (it is
# 100% first-party, no vendor sources ever land in it); alp_sdk instead
# calls the per-source helper further down, once its SOURCES are complete.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/alp-sdk-warnings.cmake)

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

# Convenience alias for downstream users (add_subdirectory() / build-tree
# consumers).  EXPORT_NAME makes the INSTALLED package's namespaced target
# match this alias too: install(EXPORT ... NAMESPACE alp::) would otherwise
# generate "alp::alp_sdk_headers" (it namespaces the raw target name, not
# this hand-written ALIAS), leaving an installed find_package(alp-sdk)
# consumer linking "alp::sdk_headers" unable to find the target (#607).
set_target_properties(alp_sdk_headers PROPERTIES EXPORT_NAME sdk_headers)
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
# Full roster of OS-agnostic chip drivers (chips/<id>/<id>.c).  This MUST
# stay a superset of every chips/<id>/<id>.c on disk minus the
# CHIP_LIST_EXCLUDED_WITH_REASON allowlist in
# scripts/check_cmake_chip_list_parity.py, which gates that in CI
# (issue #476: the list used to be a 17-entry hand subset that
# silently dropped required SoM drivers -- PMIC, clock, Ethernet PHY,
# NPU/wireless sideband -- from the plain-CMake / Yocto
# libalp_chips.a build).  Mirrors the Zephyr-side per-chip
# CONFIG_ALP_SDK_CHIP_<NAME> roster in zephyr/CMakeLists.txt /
# zephyr/Kconfig -- every driver buildable there is buildable here
# too, since none of the chips/<id>/<id>.c cores include a
# Zephyr/vendor header (they call back into <alp/peripheral.h> only).
set(ALP_SDK_CHIP_LIST
    a02yyuw a4988 act8760 ar0234 as5048a_b atecc608b atgm336h
    bme280 bmi323 bmp390 bmp581 cam_mux_pi3wvr626 cc3501e
    clk_5l35023b da9292 deepx_dxm1 drv8825 drv8833 eeprom_24c128
    es8388 gc2145 gd32g553 gd32_swd gdew0154t8 hailo_8l hx711
    icm42670 ics_43434 il3820 ili9341 ili9488 imx219 imx477
    ina236 inmp441 lis2dw12 lps22hb lsm6dso max31855 max31865
    max98357a maxim_max9295_9296 ms5611 mt6701 murata_lbee5hy2fy
    optiga_trust_m ov2640 ov5640 ov5645 ov7670 ov9281
    pca9451a pi3dbs12212 qmc5883l quectel_bg77 quectel_bg95 ra8875
    rtl8211fdi rv3028c7 semtech_sx1262 semtech_sx1276 sh1106
    ssd1306 ssd1331 st7789 tas2563 tcal9538 ti_ds90ub953_954
    tlv320aic3204 tmc2209 tmp112 tps628640 tsl2591 ublox_max_m10s
    ublox_neo_m9n ublox_sara_r5 veml7700 vl53l1x vl53l5cx wm8960
    CACHE INTERNAL "Chip drivers selectable via ALP_SDK_CHIP_<NAME>")

# Drivers deliberately withheld from libalp_chips.a (plain-CMake /
# Yocto), with a documented reason, live in the
# CHIP_LIST_EXCLUDED_WITH_REASON allowlist at the top of
# scripts/check_cmake_chip_list_parity.py -- EMPTY today, since every
# chips/<id>/<id>.c on disk is OS-agnostic and compiles against
# alp_sdk_headers alone.  That script is the single place a future
# Zephyr-only/vendor-only driver core gets excluded (with a reason),
# instead of silently falling out of ALP_SDK_CHIP_LIST.

option(ALP_BUILD_CHIPS_ONLY
    "Build the libalp_chips static archive from the enabled chip drivers (Yocto alp-chips recipe)." OFF)
foreach(_chip IN LISTS ALP_SDK_CHIP_LIST)
    string(TOUPPER "${_chip}" _chip_uc)
    option(ALP_SDK_CHIP_${_chip_uc} "Build the ${_chip} driver into libalp_chips" OFF)
endforeach()

if(ALP_BUILD_CHIPS_ONLY)
    add_library(alp_chips STATIC)
    target_link_libraries(alp_chips PUBLIC alp_sdk_headers)
    target_link_libraries(alp_chips PRIVATE alp_sdk_warnings)
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
    # See the alp_sdk_headers EXPORT_NAME comment above -- same reasoning:
    # without this, install(EXPORT ... NAMESPACE alp::) emits
    # "alp::alp_sdk" instead of "alp::sdk" for an installed package (#607).
    set_target_properties(alp_sdk PROPERTIES EXPORT_NAME sdk)
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
    # Plain-CMake mirror of the Zephyr Kconfig CONFIG_ALP_STATUS_DESCRIPTIONS
    # (default y) -- src/status_strings.c reads the same macro name on
    # every OS backend, so define it here when the option is ON instead of
    # introducing a second feature-test macro just for this build mode.
    if(ALP_SDK_STATUS_DESCRIPTIONS)
        target_compile_definitions(alp_sdk PRIVATE CONFIG_ALP_STATUS_DESCRIPTIONS=1)
    endif()
endif()

# Common helpers (always built).
add_subdirectory(src/common)

# OS backend.  Also add_subdirectory()s vendors/<som>/ on demand
# (ALP_SOM=aen/v2n), which target_sources()s straight into alp_sdk.
add_subdirectory(src/${ALP_OS})

# Vendor wrappers — added on demand by the OS backend's CMakeLists.

if(NOT DEFINED ZEPHYR_BASE)
    # First-party-only strict-warning profile (issue #634): applied
    # per-source (never a blanket target_link_libraries) so the
    # vendors/<som>/*.c wrapper sources just pulled into alp_sdk above stay
    # untouched -- see cmake/alp-sdk-warnings.cmake for the full rationale.
    # Must run AFTER add_subdirectory(src/${ALP_OS}) so alp_sdk's SOURCES
    # property already includes every backend/vendor file.
    alp_sdk_apply_strict_warnings(alp_sdk "${CMAKE_CURRENT_SOURCE_DIR}/src/")
endif()

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
#
# Issue #607: the config/version files were (a) generated into the
# BUILD tree only -- nothing ever installed them, nor the
# exported-targets file the config's `include(...)` line expects, so
# an installed SDK could never satisfy `find_package(alp-sdk 0.9
# REQUIRED)` -- and (b) named with the CamelCase "AlpSdk" package
# name, which `find_package(<name>)`'s file-name convention
# (`<name>Config.cmake` / `<lowercased-name>-config.cmake`) never
# matches for a project named "alp-sdk": CMake would look for
# "alp-sdk-config.cmake", not "AlpSdkConfig.cmake".  Renamed to match
# PROJECT_NAME.  GNUInstallDirs must be included before
# configure_package_config_file() so the install destination honours
# ${CMAKE_INSTALL_LIBDIR} (lib vs lib64) instead of a hardcoded "lib".
# ---------------------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(ALP_SDK_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
    CACHE STRING "Install location of the alp-sdk-config*.cmake package files")

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/alp-sdk-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config.cmake
    INSTALL_DESTINATION ${ALP_SDK_INSTALL_CMAKEDIR}
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# Install rules.  Only emitted for the shared/standalone build
# (ALP_SDK_BUILD_SHARED) so the add_subdirectory() flow embedded
# consumers use is unchanged -- they never invoke `install`.  This is
# what lets the meta-alp-sdk Yocto recipe package libalp_sdk.so + the
# <alp/*> headers into the target sysroot (so e.g. the alp_perception
# ROS node's find_library(alp_sdk) / find_path(alp/peripheral.h)
# resolve), AND what makes an installed SDK a real
# `find_package(alp-sdk CONFIG)` package for external CMake consumers
# (issue #607).
if(ALP_SDK_BUILD_SHARED AND NOT DEFINED ZEPHYR_BASE)
    install(TARGETS alp_sdk alp_sdk_headers
        EXPORT AlpSdkTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    # alp/chips/ headers are owned by the alp-chips package; exclude them
    # so a sysroot with both alp-sdk and alp-chips has no file collision.
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/alp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        PATTERN "chips" EXCLUDE)

    # Exported-targets file (alp-sdk-targets.cmake) the generated
    # alp-sdk-config.cmake `include(... OPTIONAL)`s, plus the config +
    # version files themselves -- all three previously existed only in
    # the build tree.
    install(EXPORT AlpSdkTargets
        FILE ${PROJECT_NAME}-targets.cmake
        NAMESPACE alp::
        DESTINATION ${ALP_SDK_INSTALL_CMAKEDIR})
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config-version.cmake
        DESTINATION ${ALP_SDK_INSTALL_CMAKEDIR})
endif()
