# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_cold_chain)

set(CC_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/ai/cold-chain-monitor/src)
target_include_directories(app PRIVATE ${CC_SRC})
target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/test_cold_chain.c
    ${CC_SRC}/cold_chain.c
)
