# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_update_log)
target_sources(app PRIVATE src/test_update_log.c)
# The test drives the engine directly via its module-private header, which
# resolves its own siblings (update_log/store.h, update_log/sha256.h)
# relative to the SDK's src/ root -- put that on the test's include path.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
