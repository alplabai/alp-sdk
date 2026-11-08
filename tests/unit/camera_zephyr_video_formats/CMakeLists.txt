# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_camera_zephyr_video_formats)
target_sources(app PRIVATE src/test_camera_zephyr_video_formats.c)
