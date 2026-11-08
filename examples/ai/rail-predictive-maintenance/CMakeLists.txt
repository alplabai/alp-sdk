# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rail_predictive_maintenance LANGUAGES C)

# _GNU_SOURCE makes M_PI visible under the host glibc (native_sim).
# The M55 cross-compiler sees M_PI via the #ifndef guard in the sources.
target_compile_definitions(app PRIVATE _GNU_SOURCE)

target_sources(app PRIVATE
    src/main.c
    src/rail_features.c
    src/rail_position.c
)
