# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

execute_process(
    COMMAND ${PYTHON_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/alp_project.py
            --input  ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --output ${CMAKE_CURRENT_BINARY_DIR}/alp.conf
            --emit zephyr-conf
    RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "alp_project.py failed (rc=${rc})")
endif()
set(OVERLAY_CONFIG ${CMAKE_CURRENT_BINARY_DIR}/alp.conf)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(drone_autopilot LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    src/autopilot.c
    src/sbus.c
    src/mixer.c
)
