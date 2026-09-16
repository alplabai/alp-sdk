# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_audio_out_start_defer)
target_sources(app PRIVATE
    src/fake_i2s.c
    src/test_audio_out_start_defer.c)
