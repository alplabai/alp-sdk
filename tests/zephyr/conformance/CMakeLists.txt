# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_conformance_test LANGUAGES C)

# Single-file suite by design: the per-class conf_class_t table plus
# the generic contract runners all live in src/main.c so the "add
# vendor N+1 = add one table row" property stays visible in one place.
target_sources(app PRIVATE
    src/main.c
)
