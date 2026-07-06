# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_motion_features)

set(MOT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/ai/wearable-activity-fall/src)
target_include_directories(app PRIVATE ${MOT_SRC})
target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/test_motion_features.c
    ${MOT_SRC}/motion_features.c
)
