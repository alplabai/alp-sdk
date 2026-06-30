# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_acoustic_features)

set(ACO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/audio/acoustic-anomaly-wind-turbine/src)
target_include_directories(app PRIVATE ${ACO_SRC})
target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/test_acoustic_features.c
    ${ACO_SRC}/acoustic_features.c
)
