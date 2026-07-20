# SPDX-License-Identifier: Apache-2.0

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
project(etl_fixed_containers LANGUAGES C CXX)

target_sources(app PRIVATE src/main.cpp)

# Zephyr's minimal C++ library (lib/cpp/minimal/include/new, the
# active <new> on native_sim when CONFIG_REQUIRES_FULL_LIBCPP is off
# -- see prj.conf) ships a stub <new> that satisfies ETL's
# `__has_include(<new>)` probe (etl/placement_new.h) WITHOUT actually
# declaring placement `operator new(size_t, void*)`.  ETL then
# assumes the standard library supplies placement new and skips its
# own fallback definition, so etl::vector::push_back() / etl::map's
# node construction fail with "no matching function for call to
# operator new(sizetype, void*)".  Forcing ETL_USING_STD_NEW=0 here
# makes ETL define its own placement-new operators instead (see
# etl/placement_new.h's `#if ETL_USING_STD_NEW` branch) -- scoped to
# this example's target only, since the fix belongs to this specific
# minimal-libcpp + vendored-ETL combination, not the shared profile
# header (metadata/library-profiles/etl/etl_profile.h) which every
# other ETL consumer also includes.
target_compile_definitions(app PRIVATE ETL_USING_STD_NEW=0)
