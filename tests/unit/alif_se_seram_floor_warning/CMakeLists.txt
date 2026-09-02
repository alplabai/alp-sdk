# SPDX-License-Identifier: Apache-2.0
#
# White-box native_sim coverage for the SERAM-floor diagnostic warning
# (issue #1700 / ADR-0030) in src/backends/soc_info/alif_se.c -- see
# src/test_alif_se_seram_floor.c's header comment for the full design.
#
# Deliberately does NOT set CONFIG_ALP_SDK=y: alif_se.c is normally
# gated behind CONFIG_ALP_SDK_SOC_INFO_ALIF_SE, which `depends on` the
# AEN801/E8-only hal_alif Kconfig symbol HAS_ALIF_SE_SERVICES and so
# can never be selected on native_sim through the real zephyr_library
# path -- same trap tests/unit/se_cryptocell_hash_bounds/CMakeLists.txt
# documents for the same #include-the-real-TU technique. Instead this
# app supplies the <alp/...> headers directly and fakes <se_service.h>
# (see fake_se_service_include/). alif_se.c's own quoted
# #include "soc_info_ops.h" resolves against its own directory
# (src/backends/soc_info/), so no extra include path is needed for it.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_alif_se_seram_floor)

get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fake_se_service_include # stand-in <se_service.h>
    ${ALP_SDK_ROOT}/include                              # <alp/...>
)

target_sources(app PRIVATE src/test_alif_se_seram_floor.c)
