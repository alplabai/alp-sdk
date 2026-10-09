# SPDX-License-Identifier: Apache-2.0
#
# White-box native_sim coverage for the Alif SE STOP / STANDBY backend (#2784, U7).
#
# Like power_alif_dcdc_bounds this app does NOT set CONFIG_ALP_SDK=y: the real
# src/backends/power/alif_se_power.c is #included by the test source (its
# Kconfig, CONFIG_ALP_SDK_POWER_ALIF_SE, depends on the E8-only hal_alif symbol
# HAS_ALIF_SE_SERVICES and can never be selected on native_sim), against a fake
# <se_service.h> with the complete off_profile_t and a recording fake of the
# silicon seam (alif_se_power_hw.h) and of the som_power API it calls.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_power_alif_se)

get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fake_se_service_include # stand-in <se_service.h>
    ${ALP_SDK_ROOT}/include                              # <alp/...>
    ${ALP_SDK_ROOT}/src/backends/power                   # power_ops.h, som_power.h
)

if(ALP_TEST_ALLOW_DEBUGGER)
    target_compile_definitions(app PRIVATE CONFIG_ALP_SDK_POWER_ALIF_SE_ALLOW_DEBUGGER=1)
endif()

# The bench scratch build: the clock restore writes its progress to the BOOT diag slot.
target_compile_definitions(app PRIVATE CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH=1)

target_sources(app PRIVATE src/test_power_alif_se.c)
