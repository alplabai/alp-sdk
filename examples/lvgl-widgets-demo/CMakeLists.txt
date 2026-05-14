# SPDX-License-Identifier: Apache-2.0
#
# CMakeLists.txt for the lvgl-widgets-demo example.  Invokes the
# SDK's project loader at configure time so the generated alp.conf
# overlays prj.conf.
cmake_minimum_required(VERSION 3.20.0)

# Run the SDK project loader BEFORE find_package(Zephyr ...) so the
# generated overlay is in OVERLAY_CONFIG when Zephyr starts parsing
# Kconfig.  See examples/i2c-scanner/CMakeLists.txt for the canonical
# pattern this example follows.
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
project(lvgl_widgets_demo LANGUAGES C)

target_sources(app PRIVATE src/main.c)
