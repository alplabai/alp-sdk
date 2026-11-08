# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_dispatch_close_cancels_waits)

# The *_ops.h headers are the internal (non-public) ABI between the
# dispatchers and their backends -- this test registers its own
# priority-255 test doubles against them, isolated in its own executable
# so they cannot hijack arbitration in any other suite.
target_sources(app PRIVATE src/test_dispatch_close_cancels_waits.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
