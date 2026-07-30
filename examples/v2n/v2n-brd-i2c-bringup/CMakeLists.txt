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

alp_sdk_zephyr_conf(m33_sm)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(v2n_brd_i2c_bringup LANGUAGES C)

target_sources(app PRIVATE src/main.c)
