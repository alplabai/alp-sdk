# SPDX-License-Identifier: Apache-2.0
#
# CMakeLists.txt for the lvgl-widgets-demo example.  The generated
# alp.conf (from board.yaml) overlays prj.conf via EXTRA_CONF_FILE.
cmake_minimum_required(VERSION 3.20.0)

# Auto-apply the RK055HDMIPI4MA0 DSI shield on the AEN M55-HP board targets
# (`alp_e1m_aen801_m55_hp/...`, `alp_e1m_aen803_m55_hp/...`): the E1M-EVK
# has no panel on its own, and the shield provides the `zephyr,display`
# chosen node and `alp-display0` alias alp_display_open() binds to (#2885,
# the same panel examples/connectivity/iot-dashboard uses, #2173).  Skipped
# when the caller already set SHIELD, so a different panel can be swapped
# in; inert on every other target, including native_sim.  BOARD is read
# from the environment or the sysbuild cache when it is not a -D entry --
# see examples/connectivity/iot-dashboard/CMakeLists.txt for why.
if(NOT DEFINED BOARD AND DEFINED ENV{BOARD})
    set(BOARD $ENV{BOARD})
endif()
if(NOT DEFINED BOARD AND DEFINED SYSBUILD_CACHE AND EXISTS "${SYSBUILD_CACHE}")
    file(STRINGS "${SYSBUILD_CACHE}" _alp_sysbuild_board_line REGEX "^BOARD:STRING=")
    if(_alp_sysbuild_board_line)
        string(REGEX REPLACE "^BOARD:STRING=" "" BOARD "${_alp_sysbuild_board_line}")
    endif()
endif()
if(DEFINED BOARD AND BOARD MATCHES "^alp_e1m_aen80[13]_m55_hp(/|$)" AND NOT DEFINED SHIELD)
    list(APPEND SHIELD e1m_evk_rk055hdmipi4ma0)
endif()

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
