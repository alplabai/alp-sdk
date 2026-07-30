# SPDX-License-Identifier: Apache-2.0
#
# CMakeLists for the Cortex-M33-SM / Zephyr slice of the rpmsg-v2n
# example.
#
# This is the build entry point Zephyr's machinery picks up when the
# orchestrator dispatches the M33 slice (board target
# `alp_e1m_v2n101_m33_sm`).  `alp_sdk_zephyr_conf()` renders this
# slice's `cores.m33_sm.*` block from ../board.yaml into a Kconfig
# fragment that gets layered over prj.conf via EXTRA_CONF_FILE.

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

alp_sdk_zephyr_conf(m33_sm BOARD_YAML ${CMAKE_CURRENT_SOURCE_DIR}/../board.yaml)

# Generate <alp/system_ipc.h> from the board.yaml `ipc:` block.  The
# producer (src/main.c) opens RPC channels via the auto-emitted macros
# (ALP_IPC_<NAME>_NAME, _ADDR, _SIZE, ...), so the header must be on
# the include path before any source compiles.
alp_sdk_ipc_contract_header(BOARD_YAML ${CMAKE_CURRENT_SOURCE_DIR}/../board.yaml)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rpmsg_v2n_m33_sm LANGUAGES C)

# Expose the orchestrator's generated/ directory (still emitted above for
# the other slices' tooling; src/main.c itself no longer includes
# <alp/system_ipc.h> -- see its file header, alp-sdk #683 Phase 1).
zephyr_include_directories(${CMAKE_BINARY_DIR}/generated)

# METAL_MAX_DEVICE_REGIONS -- number of memory regions shared between
# cores.  Default is 1 (the vrings/rpmsg buffers); this app's
# platform_init() registers a SECOND region for the resource table, so it
# must be redefined here (matches upstream's openamp_linux_zephyr sample).
zephyr_compile_definitions(METAL_MAX_DEVICE_REGIONS=2)

target_include_directories(app PRIVATE ${LIBMETAL_INCLUDE_DIR} ${OPENAMP_INCLUDE_DIR}
                            ${PLATFORM_DIR})
target_include_directories(app PRIVATE src)

target_sources(app PRIVATE src/main.c src/resource_table.c)
