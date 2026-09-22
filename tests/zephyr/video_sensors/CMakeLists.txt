# SPDX-License-Identifier: Apache-2.0
#
# A real runtime ztest for ov9281.c against an I2C emulator (ov9281_emul.c) -- see testcase.yaml.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_video_sensors_test)

target_sources(app PRIVATE src/ov9281_emul.c src/ov9281_test.c)
