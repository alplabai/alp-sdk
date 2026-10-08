# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_rv3028c7_dispatch_ack)

# Link the REAL driver TU (#1623): rv3028c7.c calls exactly two
# external symbols, alp_i2c_write() and alp_i2c_write_read(), and
# alp_i2c_t is opaque (include/alp/peripheral.h) -- so a small in-TU
# fake register file is enough to drive the actual dispatch/ack path,
# no chip-specific fake backend needed.
target_sources(app PRIVATE
    src/test_rv3028c7_dispatch_ack.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../chips/rv3028c7/rv3028c7.c
)
# Only the public <alp/...> header surface -- deliberately NOT
# CONFIG_ALP_SDK=y / the alp_sdk zephyr library, which would pull in
# the real alp_i2c_write()/alp_i2c_write_read() this test replaces
# with its own fake register file (multiple-definition link error).
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
