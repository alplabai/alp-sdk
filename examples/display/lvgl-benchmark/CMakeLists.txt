# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# PYTHON_EXECUTABLE is only set after find_package(Zephyr);
# we need a Python interpreter at configure time *before*
# that, so resolve one ourselves here.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Emit into a generated/ subdir so the file escapes Zephyr's
# ${APPLICATION_BINARY_DIR}/*.conf GLOB, and APPEND (don't overwrite)
# OVERLAY_CONFIG so a per-example native_sim.conf can also layer in.
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
list(APPEND OVERLAY_CONFIG ${_alp_generated})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lvgl_benchmark LANGUAGES C)

set(LVGL_DIR ${ZEPHYR_LVGL_MODULE_DIR})
# The benchmark demo internally calls lv_demo_widgets() to drive
# the same scene set the widgets demo paints, so we pull in the
# full widgets tree alongside the benchmark sources.
file(GLOB LV_BENCH_SRC
    ${LVGL_DIR}/demos/benchmark/*.c
    ${LVGL_DIR}/demos/benchmark/assets/*.c
    ${LVGL_DIR}/demos/widgets/*.c
    ${LVGL_DIR}/demos/widgets/assets/*.c
)
target_sources(app PRIVATE
    src/main.c
    ${LV_BENCH_SRC}
    ${LVGL_DIR}/demos/lv_demos.c
)
target_include_directories(app PRIVATE
    ${LVGL_DIR}/demos
    ${LVGL_DIR}/demos/benchmark
    ${LVGL_DIR}/demos/widgets
)
