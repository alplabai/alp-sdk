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
            --emit zephyr-conf --core m55_hp
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

# Catch2's amalgamated single-TU distribution is vendored in-tree at
# vendors/catch2/ -- see that directory's README.md for the why. The
# alp-sdk Zephyr module (zephyr/CMakeLists.txt) compiles
# catch_amalgamated.cpp and adds its include path automatically because
# board.yaml's `libraries: [catch2]` sets CONFIG_ALP_SDK_CATCH2_VENDORED=y;
# nothing to wire here beyond this app's own source.
target_sources(app PRIVATE src/main.cpp)

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
