# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_status_strings_test LANGUAGES C)

target_sources(app PRIVATE src/test_status_strings.c)
