# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nanopb_encode_decode LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    src/device_status.pb.c
)

# nanopb's zephyr/module.yml declares `cmake-ext`/`kconfig-ext` (an
# external-glue extension point alp-sdk does not wire centrally yet), so
# board.yaml's `libraries: [nanopb]` only carries the
# CONFIG_ALP_NANOPB_SW=y SW-fallback marker -- it does not, by itself, pull
# the upstream runtime into the build.  Zephyr's module loader still sets
# ${ZEPHYR_NANOPB_MODULE_DIR} for every discovered module regardless of the
# cmake-ext flag (see zephyr/cmake/modules/zephyr_module.cmake), so this
# example compiles the three runtime .c files directly against that path.
if(NOT DEFINED ZEPHYR_NANOPB_MODULE_DIR)
    message(FATAL_ERROR
        "ZEPHYR_NANOPB_MODULE_DIR not set -- pass the nanopb module path via "
        "-DZEPHYR_MODULES=<...>;<path-to>/modules/lib/nanopb (see README.md).")
endif()

target_sources(app PRIVATE
    ${ZEPHYR_NANOPB_MODULE_DIR}/pb_common.c
    ${ZEPHYR_NANOPB_MODULE_DIR}/pb_encode.c
    ${ZEPHYR_NANOPB_MODULE_DIR}/pb_decode.c
)
target_include_directories(app PRIVATE ${ZEPHYR_NANOPB_MODULE_DIR})
