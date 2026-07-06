# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_xhci_core_test LANGUAGES C)
# xhci_core.c is pure C; compile it directly into the test (no DT/MMIO needed).
target_sources(app PRIVATE
  src/test_xhci_core.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/usb/uhc/xhci_core.c)
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/usb/uhc)
