# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_audio_in_enomsg_timeout)
target_sources(app PRIVATE
    src/fake_dmic.c
    src/test_audio_in_enomsg_timeout.c)
