# SPDX-License-Identifier: Apache-2.0
#
# Build-only guard for src/backends/power/alif_aipm_gen2.h (issue #2784):
# compiling it on native_sim runs its _Static_asserts, and src/aipm_twin.c
# asserts every ALP_AIPM_GEN2_* value equals hal_alif's gen2 aipm.h twin
# (the SVD half is tests/scripts/test_alif_aipm_gen2_svd.py).  Deliberately does NOT set
# CONFIG_ENSEMBLE_GEN2 (src/aipm_twin.c defines it locally, in one TU, to
# compare against hal_alif's own gen2 aipm.h).
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_power_alif_aipm_gen2)

get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)

# hal_alif is a mandatory west module (west.yml); fail loudly if it is
# missing so the twin check can never be silently skipped.
if(NOT DEFINED ZEPHYR_ALIF_MODULE_DIR)
  message(FATAL_ERROR "hal_alif module not found: cannot build the aipm.h twin check")
endif()

target_include_directories(app PRIVATE
    ${ALP_SDK_ROOT}/src/backends/power
    ${ZEPHYR_ALIF_MODULE_DIR}/se_services/include # hal_alif aipm.h (twin check only)
)

target_sources(app PRIVATE src/test_power_alif_aipm_gen2.c src/aipm_twin.c)
