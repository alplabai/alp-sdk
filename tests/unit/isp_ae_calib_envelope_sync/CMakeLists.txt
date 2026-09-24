# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_isp_ae_calib_envelope_sync)

# The functions under test are dependency-free integer arithmetic and plain
# struct field copies (see src/test_isp_ae_calib_envelope_sync.c for why
# both are same-file mirrors rather than #includes of the real sources) --
# no storage/dsp backend, no dispatcher, no mocked HW.
target_sources(app PRIVATE src/test_isp_ae_calib_envelope_sync.c)
