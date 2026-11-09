# SPDX-License-Identifier: Apache-2.0
#
# Register-model native_sim test for zephyr/drivers/ipm/ipm_arm_mhuv2.c's
# send path (alp-sdk #2983).  #includes the real driver TU (CONFIG_ARM_MHUV2
# left unset, so no DT instances are expanded) and drives it through its
# ipm_driver_api against a RAM-backed MHUV2_SND register block.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_ipm_mhuv2_send)

get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)

target_include_directories(app PRIVATE ${ALP_SDK_ROOT}/zephyr/drivers/ipm)
target_sources(app PRIVATE src/test_ipm_mhuv2_send.c)

# The driver needs the IPM poll API from the alp-sdk Zephyr patch; a plain
# upstream tree (pr-twister) lacks it, so the suite builds as a skip there.
file(READ ${ZEPHYR_BASE}/include/zephyr/drivers/ipm.h ALP_IPM_H)
string(FIND "${ALP_IPM_H}" "poll_out" ALP_IPM_HAS_POLL)
if(ALP_IPM_HAS_POLL EQUAL -1)
  target_compile_definitions(app PRIVATE ALP_TEST_IPM_POLL_API_MISSING=1)
endif()
