# SPDX-License-Identifier: Apache-2.0
#
# CMakeLists for the Cortex-M33-SM / Zephyr slice of the rpmsg-v2n
# example.
#
# This is the build entry point Zephyr's machinery picks up when the
# orchestrator dispatches the M33 slice (board target
# `alp_e1m_v2n101_m33_sm`).  alp_project.py renders this slice's
# `cores.m33_sm.*` block from ../board.yaml into a Kconfig fragment
# that gets layered over prj.conf via OVERLAY_CONFIG.

cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------
# 1. Resolve the ALP SDK root.
# ALP_SDK_ROOT lets a customer point this example at an out-of-tree
# SDK checkout; the fallback walks up four levels
# (examples/multicore/<name>/<core> -> examples/multicore/<name>
# -> examples/multicore -> examples -> repo root) so an in-tree
# `west build` Just Works.
# ---------------------------------------------------------------------
if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../.. ABSOLUTE)
endif()

# ---------------------------------------------------------------------
# 2. Python3 BEFORE Zephyr -- alp_project.py renders this slice's
# board.yaml view into a Kconfig fragment that Zephyr then layers
# over prj.conf via OVERLAY_CONFIG.  find_package(Python3 ...) MUST
# run before find_package(Zephyr ...) because Zephyr's CMake
# machinery pins a Python interpreter the moment it imports.
# ---------------------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_alp_generated ${CMAKE_BINARY_DIR}/generated/alp.conf)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${ALP_SDK_ROOT}/scripts/alp_project.py
            --input ${CMAKE_CURRENT_SOURCE_DIR}/../board.yaml
            --emit zephyr-conf --core m33_sm
            --output ${_alp_generated}
    RESULT_VARIABLE _alp_rv
    OUTPUT_VARIABLE _alp_stdout
    ERROR_VARIABLE  _alp_stderr
)
if(NOT _alp_rv EQUAL 0)
    message(FATAL_ERROR
        "alp_project.py failed (rv=${_alp_rv}); "
        "check ${CMAKE_CURRENT_SOURCE_DIR}/../board.yaml.\n"
        "stderr: ${_alp_stderr}")
endif()

list(APPEND OVERLAY_CONFIG ${_alp_generated})

# Generate <alp/system_ipc.h> from the board.yaml `ipc:` block.  The
# producer (src/main.c) opens RPC channels via the auto-emitted macros
# (ALP_IPC_<NAME>_NAME, _ADDR, _SIZE, ...), so the header must be on
# the include path before any source compiles.
set(_alp_ipc_header ${CMAKE_BINARY_DIR}/generated/alp/system_ipc.h)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${ALP_SDK_ROOT}/scripts/alp_project.py
            --input ${CMAKE_CURRENT_SOURCE_DIR}/../board.yaml
            --emit ipc-contract-h
            --output ${_alp_ipc_header}
    RESULT_VARIABLE _alp_ipc_rv
    OUTPUT_VARIABLE _alp_ipc_stdout
    ERROR_VARIABLE  _alp_ipc_stderr
)
if(NOT _alp_ipc_rv EQUAL 0)
    message(FATAL_ERROR
        "alp_project.py --emit ipc-contract-h failed "
        "(rv=${_alp_ipc_rv}); stderr: ${_alp_ipc_stderr}")
endif()

# ---------------------------------------------------------------------
# 3. Zephyr.
# ---------------------------------------------------------------------
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rpmsg_v2n_m33_sm LANGUAGES C)

# Expose the orchestrator's generated/ directory so the
# `#include <alp/system_ipc.h>` in src/main.c resolves.
zephyr_include_directories(${CMAKE_BINARY_DIR}/generated)

target_sources(app PRIVATE src/main.c)
