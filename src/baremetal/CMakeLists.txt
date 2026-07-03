# SPDX-License-Identifier: Apache-2.0
# Bare-metal backend for the Alp SDK.
#
# Source layout:
#   - src/common/stub_backend.c provides every public alp_* symbol
#     as ALP_ERR_NOSUPPORT.  Used as the link-complete fall-back
#     so a bare-metal app builds even with ALP_SOM=none.
#   - vendors/<som>/ pulls in vendor-specific real impls (Alif HAL
#     for ALP_SOM=aen, Renesas FSP for v2n, NXP MCUXpresso for
#     imx93) when ALP_HAS_<VENDOR>_HAL is also ON.
#
# When a vendor wrapper provides an alp_* symbol the corresponding
# stub in stub_backend.c is excluded via the
# ALP_VENDOR_OVERRIDES_PERIPHERAL compile definition the vendor
# CMakeLists sets -- avoids duplicate-symbol link errors.
#
# This file only fires on plain-CMake builds (ZEPHYR_BASE unset);
# Zephyr-module mode pulls in src/zephyr/ via zephyr/CMakeLists.txt.

if(DEFINED ZEPHYR_BASE)
    return()
endif()

target_sources(alp_sdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub_backend.c
    # SDK lifecycle (alp_init / alp_deinit) -- OS-agnostic.
    ${CMAKE_CURRENT_SOURCE_DIR}/../init.c
    # Register-file I2C target helper (<alp/i2c_regfile.h>) -- pure
    # layer over alp_i2c_target_open, so it degrades to the stub's
    # ALP_ERR_NOSUPPORT here without any backend of its own.
    ${CMAKE_CURRENT_SOURCE_DIR}/../i2c_regfile.c
)

if(ALP_SOM STREQUAL "aen")
    add_subdirectory(${CMAKE_SOURCE_DIR}/vendors/alif vendors_alif)
elseif(ALP_SOM STREQUAL "v2n")
    add_subdirectory(${CMAKE_SOURCE_DIR}/vendors/renesas-rzv2n vendors_renesas_rzv2n)
endif()
