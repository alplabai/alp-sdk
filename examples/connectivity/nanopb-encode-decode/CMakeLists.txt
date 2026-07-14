# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

# Resolve the alp-sdk root.  In-tree the SDK is the example's
# grandparent directory; out-of-tree customers point ALP_SDK_ROOT
# at their checkout.
if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_alp_generated ${CMAKE_BINARY_DIR}/generated/alp.conf)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${ALP_SDK_ROOT}/scripts/alp_project.py
            --input ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --emit zephyr-conf
            --output ${_alp_generated}
    RESULT_VARIABLE _alp_rv
    OUTPUT_VARIABLE _alp_stdout
    ERROR_VARIABLE  _alp_stderr
)
if(NOT _alp_rv EQUAL 0)
    message(FATAL_ERROR
        "alp_project.py failed (rv=${_alp_rv}); "
        "check ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml.\n"
        "stderr: ${_alp_stderr}")
endif()

# Layer generated CONFIG_* over prj.conf via OVERLAY_CONFIG.
list(APPEND OVERLAY_CONFIG ${_alp_generated})

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
