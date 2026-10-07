# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_jpeg_zero_caps_exempt)

# jpeg_ops.h is the internal (non-public) ABI between src/jpeg_dispatch.c
# and a jpeg backend -- this test registers its own priority-255 test
# double against it, isolated in its own executable so it cannot hijack
# arbitration in tests/unit/jpeg_registry's suite (see the file comment).
target_sources(app PRIVATE src/test_jpeg_zero_caps_exempt.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
