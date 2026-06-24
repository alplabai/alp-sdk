# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_i2s_amp_alif LANGUAGES C)
target_sources(app PRIVATE src/main.c)
