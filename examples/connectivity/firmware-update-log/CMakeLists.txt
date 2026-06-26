# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(firmware_update_log)
target_sources(app PRIVATE src/main.c)
# ACT 2 drives the update-log engine through its module-private header, which
# resolves its siblings (update_log/store.h) relative to the SDK src/ root --
# put that on the include path, the same way tests/unit/update_log does.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
