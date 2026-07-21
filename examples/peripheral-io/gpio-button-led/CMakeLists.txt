# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

# --------------------------------------------------------------------
# board.yaml -> build/generated/alp.conf at configure time.
# --------------------------------------------------------------------
#
# Customers who copy this example out of the SDK tree point the
# ALP_SDK_ROOT env var at their checkout; in-tree we resolve it as
# the example's grandparent directory.

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

# Layer the generated CONFIG_* lines over prj.conf via Zephyr's
# EXTRA_CONF_FILE mechanism.  Must be set before find_package(Zephyr).
# `rsource` is NOT valid in .conf files (it is a Kconfig-source
# directive only), which is why prj.conf can't pull the generated
# fragment in itself.
list(APPEND EXTRA_CONF_FILE ${_alp_generated})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gpio_button_led LANGUAGES C)

target_sources(app PRIVATE src/main.c)
