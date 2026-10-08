# SPDX-License-Identifier: Apache-2.0
#
# Build-only guard for src/backends/power/alif_aipm_gen2.h (issue #2784):
# compiling the header on native_sim runs its _Static_asserts, which tie
# the gen2 aiPM memory-block / wake-event / EWIC bits to the E8 SVD
# VBAT.RET_CTRL / ANA.WKUP_CTRL fields.  Deliberately does NOT set
# CONFIG_ENSEMBLE_GEN2 and does not pull in hal_alif.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_power_alif_aipm_gen2)

get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)

target_include_directories(app PRIVATE ${ALP_SDK_ROOT}/src/backends/power)

target_sources(app PRIVATE src/test_power_alif_aipm_gen2.c)
