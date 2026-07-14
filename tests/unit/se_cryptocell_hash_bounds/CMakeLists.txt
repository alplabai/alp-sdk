# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_se_hash_bounds)

# Issue #737: pins the staging-buffer boundary check
# src/backends/security/se_cryptocell.c's se_hash_update() runs before its
# memcpy(), using the SAME shared helper (src/common/alp_checked_arith.h,
# #743) that check calls. se_cryptocell.c itself is not host-buildable here
# (it pulls in the AEN801/E8-only hal_alif se_service.h + zephyr/kernel.h),
# so this test exercises the identical arithmetic against a local mirror of
# struct se_hash_be's {used, buf[capacity]} shape instead of the TU.
target_sources(app PRIVATE src/test_se_hash_bounds.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
