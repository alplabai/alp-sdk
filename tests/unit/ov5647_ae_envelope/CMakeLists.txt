# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_ov5647_ae_envelope)

# The formula under test is dependency-free integer-macro arithmetic (see
# src/test_ov5647_ae_envelope.c for why it is a same-file mirror rather than
# an #include of the vendored header) -- no storage/dsp backend, no
# dispatcher, no mocked HW.
target_sources(app PRIVATE src/test_ov5647_ae_envelope.c)
