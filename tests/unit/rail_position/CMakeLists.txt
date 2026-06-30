# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_rail_position)

set(RAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/ai/rail-predictive-maintenance/src)
target_include_directories(app PRIVATE ${RAIL_SRC})
target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/test_rail_position.c
    ${RAIL_SRC}/rail_position.c
)
