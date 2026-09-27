# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_isp_ae_ctrl_clamp)

# The helper under test is a dependency-free static-inline function (see
# src/test_isp_ae_ctrl_clamp.c for why it is a same-file mirror rather than
# an #include of the vendored driver) -- no storage/dsp backend, no
# dispatcher, no mocked HW.
target_sources(app PRIVATE src/test_isp_ae_ctrl_clamp.c)
