# SPDX-License-Identifier: Apache-2.0
#
# Cortex-M33-SM / Zephyr slice -- the CMSIS-DSP FFT worker.

cmake_minimum_required(VERSION 3.20)

if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../.. ABSOLUTE)
endif()

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

list(APPEND EXTRA_CONF_FILE ${_alp_generated})

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

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(heterogeneous_offload_m33_sm LANGUAGES C)

# Expose the orchestrator's generated/ directory so the
# `#include <alp/system_ipc.h>` in src/main.c resolves.
zephyr_include_directories(${CMAKE_BINARY_DIR}/generated)

target_sources(app PRIVATE src/main.c)
