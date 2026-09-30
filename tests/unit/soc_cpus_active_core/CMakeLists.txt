# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_soc_cpus_active_core)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/zephyr)
target_sources(app PRIVATE src/test_soc_cpus_active_core.c)
