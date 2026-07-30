# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_sim_vision)

# This demo's fixed SIM_* buffers (src/main.cpp) sit in the M55-HP's 1 MiB
# DTCM.  The M55-HE's DTCM ends at 0x20040000 -- short of all five buffers --
# so an HE build bus-faults with no app output.  HE silicon is not this sim
# app's acceptance path (README.md); gate the build so that mismatch fails
# loudly at configure time instead of faulting at runtime.
#
# Checked post-find_package(Zephyr) against the Kconfig symbol the source
# actually keys on (CONFIG_BOARD_ALP_E1M_AEN801_M55_HP), not the raw BOARD
# CMake variable: BOARD is only defined pre-find_package when passed as
# -DBOARD=; an invocation that supplies it via $ENV{BOARD} (resolved later,
# inside boards.cmake) would otherwise hit `NOT "" MATCHES ...` here and
# hard-fail with BOARD=''.
if(NOT CONFIG_BOARD_ALP_E1M_AEN801_M55_HP)
    message(FATAL_ERROR
        "aen-sim-vision targets the M55-HP core only (its SIM_* buffers "
        "live in HP's DTCM, past the end of HE's); build with "
        "-b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp, not BOARD='${BOARD}'.")
endif()

set(NO_THREADSAFE_STATICS $<TARGET_PROPERTY:compiler-cpp,no_threadsafe_statics>)
zephyr_compile_options($<$<COMPILE_LANGUAGE:CXX>:${NO_THREADSAFE_STATICS}>)

file(GLOB app_sources src/*)
target_sources(app PRIVATE ${app_sources})
