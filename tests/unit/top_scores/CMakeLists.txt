# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_top_scores)

set(DRPAI_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/v2n/v2n-drpai-inference/src)
target_include_directories(app PRIVATE ${DRPAI_SRC})
target_sources(app PRIVATE
    src/test_top_scores.c
    ${DRPAI_SRC}/top_scores.c
)
