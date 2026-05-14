# SPDX-License-Identifier: Apache-2.0
#
# Top-level CMakeLists for the mproc-dual-os-yocto-zephyr example.
#
# This file builds ONLY the Cortex-M33/Zephyr side (`m33/main.c`).
# The Cortex-A55/Yocto-Linux side lives at `linux/CMakeLists.txt`
# and is built by `meta-alp-sdk/recipes-apps/mproc-dual-os/` inside
# the bitbake image, NOT by Zephyr.  See README.md for the cross-OS
# build flow.

cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------
# 1. Resolve the ALP SDK root.
# ALP_SDK_ROOT lets a customer point this example at an out-of-tree
# SDK checkout; the fallback walks up two levels (examples/<name>/..
# -> examples/.. -> repo root) so an in-tree `west build` Just Works.
# ---------------------------------------------------------------------
if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
endif()

# ---------------------------------------------------------------------
# 2. Python3 BEFORE Zephyr -- alp_project.py renders board.yaml into
# a Kconfig fragment that Zephyr then layers over prj.conf via
# OVERLAY_CONFIG.  find_package(Python3 ...) MUST run before
# find_package(Zephyr ...) because Zephyr's CMake machinery pins a
# Python interpreter the moment it imports, and we need our copy
# resolved first so the generator step uses the right one.
# ---------------------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_alp_generated ${CMAKE_BINARY_DIR}/generated/alp.conf)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${ALP_SDK_ROOT}/scripts/alp_project.py
            --input ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --emit zephyr-conf
            --output ${_alp_generated}
    RESULT_VARIABLE _alp_rv
    OUTPUT_VARIABLE _alp_stdout
    ERROR_VARIABLE  _alp_stderr
)
if(NOT _alp_rv EQUAL 0)
    message(FATAL_ERROR
        "alp_project.py failed (rv=${_alp_rv}); "
        "check ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml.\n"
        "stderr: ${_alp_stderr}")
endif()

# Layer the generated fragment over prj.conf.  Same shape as
# examples/mproc-mailbox -- prj.conf stays effectively empty and
# board.yaml is the single source of truth for Kconfig state.
list(APPEND OVERLAY_CONFIG ${_alp_generated})

# ---------------------------------------------------------------------
# 3. Zephyr.  Only the M33 source compiles here; the Linux consumer
# under `linux/` is intentionally invisible to this build.
# ---------------------------------------------------------------------
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mproc_dual_os LANGUAGES C)

target_sources(app PRIVATE m33/main.c)
