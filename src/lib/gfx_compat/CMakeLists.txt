# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alp Lab AB
#
# gfx_compat -- in-tree maintainer-written shim (see gfx_compat.h).
# add_subdirectory()'d from zephyr/CMakeLists.txt only -- Zephyr-module
# mode is the only consumer today (native_sim examples); a plain-CMake
# hookup can add an `if(NOT DEFINED ZEPHYR_BASE)` branch here the same
# way src/common/CMakeLists.txt does, if/when a bare-metal consumer
# needs it.

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_ALP_GFX_COMPAT_SW
    ${CMAKE_CURRENT_SOURCE_DIR}/gfx_compat.c
)
