# SPDX-License-Identifier: Apache-2.0
#
# **[UNTESTED]**: the `libhelix` west module's pinned revision failed
# to check out in this workspace (empty `modules/lib/libhelix` --
# see README.md "Module status").  This CMakeLists.txt is still
# wired the way a working checkout would need it -- `EXTRA_ZEPHYR_MODULES`
# carrying the module path -- so it's a correct template, not a build
# that currently succeeds.

cmake_minimum_required(VERSION 3.20)

# Resolve the alp-sdk root.  In-tree the SDK is the example's
# grandparent directory; out-of-tree customers point ALP_SDK_ROOT
# at their checkout.
if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
endif()

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

# Layer generated CONFIG_* over prj.conf via OVERLAY_CONFIG.
list(APPEND OVERLAY_CONFIG ${_alp_generated})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(libhelix_decode LANGUAGES C)

target_sources(app PRIVATE src/main.c)

# NOT WIRED: the libhelix west module (west.yml `path:
# modules/lib/libhelix`, extras-tier1 group) failed its pinned-revision
# checkout in this workspace (empty repo -- see README.md "Module
# status"), so there is no `mp3dec.h` anywhere to point at yet, and
# adding a speculative include path here would be misleading. Once a
# real checkout lands, wire it exactly like minimp3-decode's
# CMakeLists.txt does:
#
#   target_include_directories(app PRIVATE <module-root-or-include-dir>)
#
# (libhelix, like minimp3 and tinygsm, ships no zephyr/module.yml, so
# west alone won't add its include path even after a successful fetch.)
