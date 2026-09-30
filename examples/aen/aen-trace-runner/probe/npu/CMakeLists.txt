# SPDX-License-Identifier: Apache-2.0
#
# M55-HP NPU body-control probe (see README.md). Standalone Zephyr app; shares
# only src/vision/movenet.c + pose.c with the game, the code under test.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(trace_runner_npu_probe LANGUAGES C CXX)

target_sources(app PRIVATE src/main.c ../../src/vision/movenet.c ../../src/vision/pose.c)
