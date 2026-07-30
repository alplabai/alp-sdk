# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

# board.yaml -> build/generated/alp.conf at configure time, layered
# over prj.conf via EXTRA_CONF_FILE.  Both the emit and the append
# MUST happen before find_package(Zephyr) -- cmake/alp.cmake documents
# why, and the ALP_SDK_ROOT contract a copied-out project relies on.
if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
endif()
include(${ALP_SDK_ROOT}/cmake/alp.cmake)

alp_sdk_zephyr_conf(m55_he)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_console LANGUAGES C)

target_sources(app PRIVATE src/main.c)

# Reusable SoM bring-up helper (cc3501e_bridge_bringup): the one-call
# CC3501E bridge init.  Copied verbatim from
# examples/aen/aen-cc3501e-bringup/src/ so this example is self-contained.
# When CONFIG_ALP_SDK_CHIP_CC3501E is off (native_sim / V2N) the TU still
# compiles -- it just resolves to the stub cc3501e_bridge_bringup() that
# returns ALP_ERR_NOT_PRESENT_ON_THIS_SOC immediately.
if(CONFIG_ALP_SDK_CHIP_CC3501E)
    target_sources(app PRIVATE src/cc3501e_bridge.c)
endif()
