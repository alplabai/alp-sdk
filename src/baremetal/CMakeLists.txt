# SPDX-License-Identifier: Apache-2.0
# Bare-metal backend for the ALP SDK.
#
# v0.1: stub backend.  Every alp_*_open returns NULL with
# ALP_ERR_NOSUPPORT -- apps targeting baremetal get a link-complete
# library but no working peripherals.  Real implementations land
# v0.2 alongside vendors/alif (Alif HAL bindings) and call into the
# vendor's CMSIS-Driver layer.
#
# This file only fires on plain-CMake builds (ZEPHYR_BASE unset);
# Zephyr-module mode pulls in src/zephyr/ via zephyr/CMakeLists.txt.

if(NOT DEFINED ZEPHYR_BASE)
    target_sources(alp_sdk PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub_backend.c
    )
endif()
