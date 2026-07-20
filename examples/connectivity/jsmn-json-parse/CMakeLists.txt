# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

# Resolve the alp-sdk root.  In-tree the SDK is the example's
# grandparent directory; out-of-tree customers point ALP_SDK_ROOT
# at their checkout.  Still needed below for the vendored jsmn
# include path even though the Kconfig-emit call that used to sit
# here is gone (the planner now wires alp.conf via EXTRA_CONF_FILE).
if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(jsmn_json_parse LANGUAGES C)

target_sources(app PRIVATE src/main.c)

# jsmn ships vendored (single header, no companion .c) rather than as a
# west-fetched Zephyr module -- see vendors/jsmn/README.md for why. Add its
# include directory directly; board.yaml's `libraries: [jsmn]` only carries
# the CONFIG_ALP_JSMN_SW=y SW-fallback marker, not an include-path hook.
target_include_directories(app PRIVATE ${ALP_SDK_ROOT}/vendors/jsmn/include)
