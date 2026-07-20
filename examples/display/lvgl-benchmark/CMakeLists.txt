# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

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
