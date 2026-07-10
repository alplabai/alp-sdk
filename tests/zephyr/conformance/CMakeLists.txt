# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_conformance_test LANGUAGES C)

# Single-file suite by design: the per-class conf_class_t table plus
# the generic contract runners all live in src/main.c so the "add
# vendor N+1 = add one table row" property stays visible in one place.
#
# main.c's data-driven rows assume the REAL/emulated backend per class
# (e.g. gpio's Case B expects alp_gpio_open(99) to fail -- true for
# zephyr_drv, never true for a test double that must open ANY
# instance).  CONFIG_ALP_SDK_TESTING's priority-255 wildcard backends
# are therefore built as a SEPARATE app image (the
# alp_sdk.conformance.test_doubles scenario) that swaps main.c out for
# the per-class behavior_*.c files (src/behavior_gpio.c,
# src/behavior_uart.c, ...) instead of adding to the same binary -- the
# two suites test different, mutually-incompatible backend selections
# for the same class and must never run in one process.  main.c itself
# is untouched; only this file's source list is scenario-conditional.
# Adding class N+1's test double: add its behavior_<class>.c to the
# list below (one file per class, mirroring one testing_drv.c per
# class under src/backends/<class>/).
if(CONFIG_ALP_SDK_TESTING)
    target_sources(app PRIVATE
        src/behavior_gpio.c
        src/behavior_uart.c
        src/behavior_i2c.c
        src/behavior_spi.c
        src/behavior_adc.c
        src/behavior_can.c
        src/behavior_storage.c
    )
else()
    target_sources(app PRIVATE
        src/main.c
    )
endif()
