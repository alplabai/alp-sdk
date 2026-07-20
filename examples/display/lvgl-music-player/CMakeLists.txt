# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

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
