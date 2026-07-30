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

alp_sdk_zephyr_conf(m55_hp)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(minimp3_decode LANGUAGES C)

target_sources(app PRIVATE src/main.c)

# minimp3 is vendored (header-only, no zephyr/module.yml -- see
# vendors/minimp3/README.md) rather than fetched as a west module, so
# unlike a real Zephyr module its include directory isn't added
# automatically; wire it in explicitly.
target_include_directories(app PRIVATE ${ALP_SDK_ROOT}/vendors/minimp3/include)
