# SPDX-License-Identifier: Apache-2.0
#
# CMakeLists.txt for the lvgl-widgets-demo example.  Invokes the
# SDK's project loader at configure time so the generated alp.conf
# overlays prj.conf.
cmake_minimum_required(VERSION 3.20.0)

# Run the SDK project loader BEFORE find_package(Zephyr ...) so the
# generated overlay is in EXTRA_CONF_FILE when Zephyr starts parsing
# Kconfig.  See examples/peripheral-io/i2c-scanner/CMakeLists.txt for the canonical
# pattern this example follows.
#
# PYTHON_EXECUTABLE is only set after find_package(Zephyr); we need a
# Python interpreter at configure time *before* that, so resolve one
# ourselves here.
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
