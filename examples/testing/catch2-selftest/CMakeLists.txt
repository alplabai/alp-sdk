# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

# Resolve the alp-sdk root. In-tree the SDK is the example's
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
project(catch2_selftest LANGUAGES C CXX)

# --------------------------------------------------------------------
# Locate the west-fetched Catch2 module (west.yml's `catchorg/Catch2`
# `extras-cpp`-adjacent entry, pinned at modules/lib/catch2).
# Catch2 carries no zephyr/module.yml (it's a generic CMake project,
# not a Zephyr module), so it can't ride in via ZEPHYR_EXTRA_MODULES'
# usual CMakeLists.txt/Kconfig auto-include -- it never becomes a
# `zephyr_library()`.  Instead we scan the ZEPHYR_MODULES cache
# variable (populated from `west twister ... -x
# ZEPHYR_MODULES="<alp-sdk-root>;<path-to-catch2-checkout>"`, see the
# README) for the checkout and compile Catch2's own recommended
# "amalgamated" single-TU distribution
# (extras/catch_amalgamated.{hpp,cpp}) directly into this app.
# --------------------------------------------------------------------
set(CATCH2_MODULE_DIR "" CACHE PATH
    "Path to a Catch2 checkout containing extras/catch_amalgamated.cpp")

if(NOT CATCH2_MODULE_DIR)
    foreach(_mod ${ZEPHYR_MODULES})
        if(EXISTS ${_mod}/extras/catch_amalgamated.cpp)
            set(CATCH2_MODULE_DIR ${_mod})
        endif()
    endforeach()
endif()

if(NOT CATCH2_MODULE_DIR)
    message(FATAL_ERROR
        "catch2-selftest: no Catch2 checkout found. Fetch the "
        "`catchorg/Catch2` west module (west.yml, path "
        "modules/lib/catch2) and pass it via "
        "-x ZEPHYR_MODULES=\"<alp-sdk-root>;<path-to-catch2>\", "
        "or set -DCATCH2_MODULE_DIR=<path-to-catch2> directly.")
endif()

target_sources(app PRIVATE
    src/main.cpp
    ${CATCH2_MODULE_DIR}/extras/catch_amalgamated.cpp
)
target_include_directories(app PRIVATE ${CATCH2_MODULE_DIR}/extras)

# Tell catch_amalgamated.cpp to skip its own default main() -- src/main.cpp
# supplies one so it can print the SDK's `[catch2-selftest] done` marker.
#
# CATCH_CONFIG_NO_POSIX_SIGNALS: Catch2 installs its own SIGSEGV/SIGABRT/etc.
# handlers by default (FatalConditionHandler) so a crashing assertion reports
# as a test failure instead of killing the process outright. native_sim's
# POSIX arch runtime installs its OWN signal handlers to implement Zephyr's
# threading model on the host -- the two fight (segfault at startup) if
# Catch2's handlers are left enabled. Same class of fix as
# metadata/library-profiles/doctest/doctest_config.h's
# DOCTEST_CONFIG_NO_POSIX_SIGNALS.
target_compile_definitions(app PRIVATE
    CATCH_AMALGAMATED_CUSTOM_MAIN
    CATCH_CONFIG_NO_POSIX_SIGNALS
)
