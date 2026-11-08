# SPDX-License-Identifier: Apache-2.0
#
# White-box native_sim coverage for the Zephyr USB device backend's descriptor
# guard (#2932): a non-default VID/PID/bcd_device/strings must be refused with
# ALP_ERR_NOSUPPORT instead of being accepted and silently ignored.
#
# Like power_alif_se this app does NOT set CONFIG_ALP_SDK=y: the real
# src/backends/usb/zephyr_drv.c is #included by the test source with
# CONFIG_ALP_SDK_USB defined (the real symbol depends on USB_DEVICE_STACK,
# which native_sim does not provide), against a stand-in
# <zephyr/usb/usb_device.h>.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_usb_zephyr_descriptor)

get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)

target_include_directories(app BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fake_usb_include # stand-in <zephyr/usb/usb_device.h>
)
target_include_directories(app PRIVATE
    ${ALP_SDK_ROOT}/include            # <alp/...>
    ${ALP_SDK_ROOT}/src/common         # alp_errno.h, alp_slot_claim.h
    ${ALP_SDK_ROOT}/src/backends/usb   # usb_ops.h
)

target_sources(app PRIVATE src/test_usb_zephyr_descriptor.c)
