# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_current_features)

set(CURR_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/ai/motor-current-signature/src)
target_include_directories(app PRIVATE ${CURR_SRC})
target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/test_current_features.c
    ${CURR_SRC}/current_features.c
)
