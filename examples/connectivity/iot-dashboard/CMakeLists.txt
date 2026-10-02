# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Auto-apply the RK055HDMIPI4MA0 DSI shield on the AEN M55-HP board targets
# this app's board.yaml actually names (`alp_e1m_aen801_m55_hp/...` and
# `alp_e1m_aen803_m55_hp/...`) -- this app's only core (board.yaml
# `cores: m55_hp:`).  The shield wires up the full Ensemble E8
# C2-MIPI-DSI DISPLAY chain (cdc200 -> mipi_dsi -> the shared D-PHY -> the
# RK055HDMIPI4MA0/HX8394 panel) on the EVK's J6 connector -- see
# examples/aen/aen-dsi-display/CMakeLists.txt for the same shield applied
# the same way (#2173).  Skipped when the caller already set SHIELD (e.g.
# `west build -DSHIELD=...`), so a user can swap in a different panel, and
# inert on every other target, including native_sim.
#
# BOARD reaches this file differently depending on the caller:
#  - a plain (non-sysbuild) `west build -b <board>` / twister's `-p
#    <platform>` both pass it in as a `-DBOARD=...` cache entry on the
#    command line, which CMake sees before this file runs, so it is
#    already a normal CMake variable below.
#  - a caller that exports BOARD instead of passing -D is covered by the
#    $ENV{BOARD} fallback.
#  - under `--sysbuild`, which is the AEN flow's documented default (see
#    docs/_aen-runbook-section.md and scripts/bench/aen/README.md),
#    sysbuild's `share/sysbuild/cmake/modules/sysbuild_extensions.cmake`
#    does NOT forward BOARD as a `-D` cache entry to the image build at
#    all (its shared_cmake_variables_list omits it); Zephyr's own
#    cmake/modules/boards.cmake resolves BOARD from the sysbuild cache
#    target instead, via zephyr_get(), *inside* find_package(Zephyr) --
#    too late to affect the regex below, which must run before
#    find_package() for SHIELD to take effect. So for sysbuild, read
#    BOARD out of the sysbuild cache file ourselves: SYSBUILD_CACHE is
#    passed to every image sub-build as a plain `-D` cache entry
#    (sysbuild_extensions.cmake's ExternalProject_Add CMAKE_ARGS), and
#    the file it points at always carries a `BOARD:STRING=<board>/<board
#    qualifiers>` line (sysbuild_extensions.cmake's sysbuild_cache()).
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
project(iot_dashboard LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    src/dashboard_ui.c
    # Reusable SoM bring-up helper (cc3501e_bridge_bringup): the one-call
    # CC3501E bridge init this app uses so alp_wifi_open() can bind to a
    # ready bridge on E1M-AEN801 (#2112). Copyable template, same file as
    # examples/aen/aen-cc3501e-* -- see src/cc3501e_bridge.h.
    src/cc3501e_bridge.c
)
