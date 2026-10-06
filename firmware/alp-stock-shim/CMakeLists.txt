# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_stock_shim LANGUAGES C)

target_sources(app PRIVATE src/main.c)
# <alp/protocol/amp_beacon.h>: the beacon layout shared with the A55 backend.
target_include_directories(app PRIVATE ${ZEPHYR_ALP_SDK_MODULE_DIR}/include)
