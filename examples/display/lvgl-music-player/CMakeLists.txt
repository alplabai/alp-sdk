# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Resolve the alp-sdk root.  In-tree the SDK is the example's
# grandparent directory; out-of-tree customers point ALP_SDK_ROOT
# at their checkout.
if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
endif()

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
            ${ALP_SDK_ROOT}/scripts/alp_project.py
            --input  ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --output ${_alp_generated}
            --emit zephyr-conf --core m55_hp
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE _alp_stdout
    ERROR_VARIABLE  _alp_stderr
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "alp_project.py failed (rc=${rc}); "
        "check ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml.\n"
        "stderr: ${_alp_stderr}")
endif()
list(APPEND EXTRA_CONF_FILE ${_alp_generated})

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
