# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_sdhc_reset_regs_compare)

set(SDHC_PROBE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/aen/aen-sdhc-probe/src)
target_include_directories(app PRIVATE ${SDHC_PROBE_SRC})
target_sources(app PRIVATE src/test_sdhc_reset_regs_compare.c)
