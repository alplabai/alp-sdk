# SPDX-License-Identifier: Apache-2.0
#
# White-box native_sim coverage for the Alif SE DC-DC rail-voltage guard
# (issue #1626) in src/backends/power/alif_se_profile.c's alif_profile_set()
# -- see this directory's src/test_power_alif_dcdc_bounds.c header comment
# for the full design.
#
# Deliberately does NOT set CONFIG_ALP_SDK=y: alif_se_profile.c is normally
# gated behind CONFIG_ALP_SDK_POWER_PROFILE_ALIF_SE, which `depends on` the
# AEN801/E8-only hal_alif Kconfig symbol HAS_ALIF_SE_SERVICES and so can
# never be selected on native_sim through the real zephyr_library path.
# Pulling in CONFIG_ALP_SDK=y would also link the whole alp_sdk
# zephyr_library (including src/power_dispatch.c) into the same image as
# the copy of alif_se_profile.c the test source #includes directly -- an
# ODR/duplicate-definition risk, the same trap
# tests/unit/se_cryptocell_hash_bounds/CMakeLists.txt documents for the
# same #include-the-real-TU technique. Instead this app supplies the
# <alp/...> headers directly and fakes <se_service.h> (see
# fake_se_service_include/).
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_power_alif_dcdc_bounds)

get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fake_se_service_include # stand-in <se_service.h>
    ${ALP_SDK_ROOT}/include                              # <alp/...>
    ${ALP_SDK_ROOT}/src/backends/power                   # power_ops.h
)

target_sources(app PRIVATE src/test_power_alif_dcdc_bounds.c)
