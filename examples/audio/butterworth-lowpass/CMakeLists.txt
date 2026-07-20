# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(butterworth_lowpass LANGUAGES C)

# _GNU_SOURCE makes M_PI visible under the host glibc (native_sim); the
# M55 cross-compiler sees M_PI via the #ifndef guard in the source.
target_compile_definitions(app PRIVATE _GNU_SOURCE)

target_sources(app PRIVATE src/main.c)
