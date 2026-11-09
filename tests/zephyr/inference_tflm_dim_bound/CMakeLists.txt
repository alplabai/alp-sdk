# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_inference_tflm_dim_bound_test LANGUAGES C)

# tflite-micro's vendored gemmlowp fixedpoint_sse.h trips
# -Wignored-attributes on x86 (native_sim); CI builds with warnings as errors.
if(CONFIG_ARCH_POSIX)
  zephyr_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-ignored-attributes>)
endif()

target_sources(app PRIVATE src/main.c)
