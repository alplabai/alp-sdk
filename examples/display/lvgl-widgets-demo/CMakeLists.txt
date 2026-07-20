# SPDX-License-Identifier: Apache-2.0
#
# CMakeLists.txt for the lvgl-widgets-demo example.  Invokes the
# SDK's project loader at configure time so the generated alp.conf
# overlays prj.conf.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lvgl_widgets_demo LANGUAGES C)

# The Zephyr lvgl module compiles only the core lvgl/src/ tree; the
# `demos/widgets/` showcase ships in lvgl but isn't auto-linked.
# Pull its sources in here so the prj.conf knob actually wires
# something into the binary.
set(LVGL_DIR ${ZEPHYR_LVGL_MODULE_DIR})
file(GLOB LV_WIDGETS_SRC
    ${LVGL_DIR}/demos/lv_demos.c
    ${LVGL_DIR}/demos/widgets/*.c
    ${LVGL_DIR}/demos/widgets/assets/*.c
)
target_sources(app PRIVATE src/main.c ${LV_WIDGETS_SRC})
target_include_directories(app PRIVATE ${LVGL_DIR}/demos ${LVGL_DIR}/demos/widgets)
