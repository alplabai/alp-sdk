# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_mproc_test LANGUAGES C)

target_sources(app PRIVATE src/main.c)

# alp_sdk.mproc.doorbell: mock MBOX controllers (src/mbox_mock.c).
if(ALP_MPROC_TEST_DOORBELL)
  target_sources(app PRIVATE src/mbox_mock.c)
  target_compile_definitions(app PRIVATE ALP_MPROC_TEST_DOORBELL)
endif()
