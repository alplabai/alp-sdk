# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

# Resolve the alp-sdk root.  In-tree the SDK is the example's
# grandparent directory; out-of-tree customers point ALP_SDK_ROOT
# at their checkout.  Still needed below for the vendored minimp3
# include path even though the Kconfig-emit call that used to sit
# here is gone (the planner now wires alp.conf via EXTRA_CONF_FILE).
if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(minimp3_decode LANGUAGES C)

target_sources(app PRIVATE src/main.c)

# minimp3 is vendored (header-only, no zephyr/module.yml -- see
# vendors/minimp3/README.md) rather than fetched as a west module, so
# unlike a real Zephyr module its include directory isn't added
# automatically; wire it in explicitly.
target_include_directories(app PRIVATE ${ALP_SDK_ROOT}/vendors/minimp3/include)
