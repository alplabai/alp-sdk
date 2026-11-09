# SPDX-License-Identifier: Apache-2.0
#
# White-box native_sim coverage for the D-cache maintenance around the SE
# service requests in src/backends/security/se_cryptocell.c (alp-sdk #2979).
# Same #include-the-real-TU technique and fake <se_service.h> as
# tests/unit/se_cryptocell_hash_bounds; here the SEND_SEAM wire paths are on
# and sys_cache_*() are intercepted by the test source.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_se_cryptocell_cache)

get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fake_se_service_include # <se_service.h>, <soc_memory_map.h>
    ${ALP_SDK_ROOT}/include
    ${ALP_SDK_ROOT}/src/common
)

target_sources(app PRIVATE src/test_se_cryptocell_cache.c)
