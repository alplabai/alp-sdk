# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# PYTHON_EXECUTABLE is only set after find_package(Zephyr);
# we need a Python interpreter at configure time *before*
# that, so resolve one ourselves here.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Emit into a generated/ subdir so the file escapes Zephyr's
# ${APPLICATION_BINARY_DIR}/*.conf GLOB, and APPEND (don't overwrite)
# EXTRA_CONF_FILE so a per-example native_sim.conf can also layer in.
set(_alp_generated ${CMAKE_BINARY_DIR}/generated/alp.conf)
execute_process(
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/alp_project.py
            --input  ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --output ${_alp_generated}
            --emit zephyr-conf
    RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "alp_project.py failed (rc=${rc})")
endif()
list(APPEND EXTRA_CONF_FILE ${_alp_generated})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(drone_hud LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    src/hud_ui.c
    src/sensors.c
)
