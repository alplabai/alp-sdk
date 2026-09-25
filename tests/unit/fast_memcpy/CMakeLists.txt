# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_fast_memcpy)

# The routine under test (src/common/alp_fast_memcpy_core.c, #2285) is a
# dependency-free OS-agnostic C file -- compile it straight into the
# test binary, same as the Zephyr memcpy() override
# (src/zephyr/fast_memcpy.c) does, but WITHOUT that wrapper, so this
# binary never defines its own memcpy() and never collides with
# native_sim's host libc.
target_sources(app PRIVATE
    src/test_fast_memcpy.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/common/alp_fast_memcpy_core.c
)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/common)

# Activate the core's test-only word-alignment guard (see
# alp_fast_memcpy_core.c) -- this target only, never a real Ensemble
# image, so a mutated alignment prologue aborts here instead of
# silently passing on a host that tolerates unaligned word accesses.
target_compile_definitions(app PRIVATE ALP_FAST_MEMCPY_CORE_ASSERT_ALIGN)
