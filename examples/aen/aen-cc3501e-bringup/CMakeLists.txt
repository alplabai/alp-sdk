# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

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
            --emit zephyr-conf --core m55_he
            --output ${_alp_generated}
    RESULT_VARIABLE _alp_rv
    OUTPUT_VARIABLE _alp_stdout
    ERROR_VARIABLE  _alp_stderr
)
if(NOT _alp_rv EQUAL 0)
    message(FATAL_ERROR "alp_project.py failed (rv=${_alp_rv})\nstderr: ${_alp_stderr}")
endif()
list(APPEND OVERLAY_CONFIG ${_alp_generated})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_cc3501e_bringup LANGUAGES C)

target_sources(app PRIVATE src/main.c)
# Reusable SoM bring-up helper (cc3501e_bridge_bringup): the one-call CC3501E bridge
# init the example + applications use. Copyable template -- see cc3501e_bridge.h.
target_sources(app PRIVATE src/cc3501e_bridge.c)
# E1M-AEN GPIO-proxy route table: strong override of the proxy backend's weak
# cc3501e_gpio_routes[] (E1M IO pin_id -> raw CC3501E GPIO). Built unconditionally;
# only consulted when CONFIG_ALP_SDK_GPIO_CC3501E_PROXY is enabled (prj.conf).
target_sources(app PRIVATE src/cc3501e_gpio_routes.c)

# Opt-in OTA streaming demo (Step 7.5 in main.c). Left OFF by default so a
# normal bring-up run never kicks a (disruptive) flash cycle.
#   -DCC3501E_OTA_DEMO=ON  -- stream an inert blob (proves the wire path only).
#   -DCC3501E_OTA_REAL=ON  -- stream the genuine signed candidate image
#                             (v0.0.4.0), so FINISH reaches a real STAGED. This
#                             compiles the shared candidate blob from firmware/
#                             so the extern resolves; it implies OTA_DEMO.
#   -DCC3501E_OTA_PROMOTE=ON -- do not stream; request the swap-reboot for an
#                             image a prior OTA_REAL run left STAGED-but-pending
#                             (unjams the slot). Implies OTA_DEMO (the call site).
option(CC3501E_OTA_DEMO "Enable the opt-in CC3501E OTA streaming demo" OFF)
option(CC3501E_OTA_REAL "OTA demo streams the real signed candidate image" OFF)
option(CC3501E_OTA_PROMOTE "OTA demo promotes an already-committed pending image" OFF)
if(CC3501E_OTA_DEMO OR CC3501E_OTA_REAL OR CC3501E_OTA_PROMOTE)
    target_compile_definitions(app PRIVATE CC3501E_OTA_DEMO)
endif()
if(CC3501E_OTA_REAL)
    target_compile_definitions(app PRIVATE CC3501E_OTA_REAL)
    target_sources(app PRIVATE
        ${ALP_SDK_ROOT}/firmware/cc3501e/hal/ti/cc3501e_ota_candidate.c)
endif()
if(CC3501E_OTA_PROMOTE)
    target_compile_definitions(app PRIVATE CC3501E_OTA_PROMOTE)
endif()
