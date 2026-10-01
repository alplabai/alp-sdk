# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

# Enable the cloud SDK symbols only when both west-pinned checkouts exist
# (`west update --group-filter +extras-cloud`); otherwise the app builds
# without them and testcase.yaml's filter skips the test.
include(${CMAKE_CURRENT_LIST_DIR}/../../../vendors/cloud-sdk-dir.cmake)
alp_cloud_sdk_dir(_aws aws-iot-device-sdk-embedded-C ALP_AWS_IOT_SDK_DIR)
alp_cloud_sdk_dir(_azure azure-sdk-for-c ALP_AZURE_IOT_SDK_DIR)
if(_aws AND _azure)
    list(APPEND CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj.conf
                          ${CMAKE_CURRENT_LIST_DIR}/cloud.conf)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_cloud_sdks_test LANGUAGES C)

target_sources(app PRIVATE src/main.c)
