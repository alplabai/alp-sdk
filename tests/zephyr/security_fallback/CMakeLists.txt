# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_security_fallback_test LANGUAGES C)

target_sources(app PRIVATE src/main.c)

# The fake backends register REAL alp_security_ops_t vtables into the
# production `security` class section, so the test needs the internal
# dispatcher<->backend ABI header (deliberately not public).
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/backends/security)
