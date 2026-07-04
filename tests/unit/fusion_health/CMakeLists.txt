# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_fusion_health)

set(FUS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/ai/multimodal-fusion-pdm/src)
target_include_directories(app PRIVATE ${FUS_SRC})
target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/test_fusion_health.c
    ${FUS_SRC}/fusion_health.c
)
