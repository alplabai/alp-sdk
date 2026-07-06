# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_defect_map)

set(DEF_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/ai/visual-defect-detection/src)
target_include_directories(app PRIVATE ${DEF_SRC})
target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/test_defect_map.c
    ${DEF_SRC}/defect_map.c
)
