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
list(APPEND EXTRA_CONF_FILE ${_alp_generated})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_cc3501e_gpio LANGUAGES C)

target_sources(app PRIVATE src/main.c)
# Reusable SoM bring-up helper (cc3501e_bridge_bringup): the one-call CC3501E bridge
# init the example + applications use. Copyable template -- see cc3501e_bridge.h.
target_sources(app PRIVATE src/cc3501e_bridge.c)
# E1M-AEN GPIO-proxy route table: strong override of the proxy backend's weak
# cc3501e_gpio_routes[] (E1M IO pin_id -> raw CC3501E GPIO). Built unconditionally;
# only consulted when CONFIG_ALP_SDK_GPIO_CC3501E_PROXY is enabled (prj.conf).
target_sources(app PRIVATE src/cc3501e_gpio_routes.c)
