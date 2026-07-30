# SPDX-License-Identifier: Apache-2.0
#
# CMakeLists for the Cortex-M55-HP / Zephyr slice of rpmsg-aen.

cmake_minimum_required(VERSION 3.20)

# board.yaml -> build/generated/alp.conf at configure time, layered
# over prj.conf via EXTRA_CONF_FILE.  Both the emit and the append
# MUST happen before find_package(Zephyr) -- cmake/alp.cmake documents
# why, and the ALP_SDK_ROOT contract a copied-out project relies on.
if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../.. ABSOLUTE)
endif()
include(${ALP_SDK_ROOT}/cmake/alp.cmake)

alp_sdk_zephyr_conf(m55_hp BOARD_YAML ${CMAKE_CURRENT_SOURCE_DIR}/../board.yaml)

# Generate <alp/system_ipc.h> from the board.yaml `ipc:` block.  The
# producer (src/main.c) opens RPC channels via the auto-emitted macros
# (ALP_IPC_<NAME>_NAME, _ADDR, _SIZE, ...), so the header must be on
# the include path before any source compiles.
alp_sdk_ipc_contract_header(BOARD_YAML ${CMAKE_CURRENT_SOURCE_DIR}/../board.yaml)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rpmsg_aen_m55_hp LANGUAGES C)

# Expose the orchestrator's generated/ directory so the
# `#include <alp/system_ipc.h>` in src/main.c resolves.
zephyr_include_directories(${CMAKE_BINARY_DIR}/generated)

target_sources(app PRIVATE src/main.c)
