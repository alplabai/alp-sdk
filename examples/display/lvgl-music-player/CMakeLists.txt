# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# PYTHON_EXECUTABLE is only set after find_package(Zephyr);
# we need a Python interpreter at configure time *before*
# that, so resolve one ourselves here.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

execute_process(
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/alp_project.py
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
project(lvgl_music_player LANGUAGES C)

set(LVGL_DIR ${ZEPHYR_LVGL_MODULE_DIR})
# The music demo uses lv_demo_args_init() from the top-level
# lv_demos.c shared helper, so include it alongside the music
# sources.  Plain GLOB on demos/*.c would also pull in the
# widgets/benchmark dispatch glue we don't want.
file(GLOB LV_MUSIC_SRC
    ${LVGL_DIR}/demos/music/*.c
    ${LVGL_DIR}/demos/music/assets/*.c
)
target_sources(app PRIVATE
    src/main.c
    ${LV_MUSIC_SRC}
    ${LVGL_DIR}/demos/lv_demos.c
)
target_include_directories(app PRIVATE ${LVGL_DIR}/demos ${LVGL_DIR}/demos/music)
