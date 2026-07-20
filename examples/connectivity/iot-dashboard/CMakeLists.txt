# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# PYTHON_EXECUTABLE is only set after find_package(Zephyr);
# we need a Python interpreter at configure time *before*
# that, so resolve one ourselves here.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Place alp.conf in the build-tree `generated/` subdir, NOT at the
# build-dir root.  Zephyr's kconfig.cmake greps ${APPLICATION_BINARY_DIR}/*.conf
# at the END of the merge-config-files list (cmake/modules/kconfig.cmake line 303
# in v4.4.0), so any .conf file dropped in the build-root wins over every
# `EXTRA_CONF_FILE=...` / `OVERLAY_CONFIG=...` overlay passed via
# `extra_args` in testcase.yaml -- including the native_sim.conf overlay
# that disables CONFIG_MBEDTLS to side-step the Zephyr v4.4 / mbedtls 3.6
# `ssl_misc.h` include-order bug.  Burying the file one level deeper keeps
# the overlay chain working as expected.
set(_alp_generated ${CMAKE_CURRENT_BINARY_DIR}/generated/alp.conf)
execute_process(
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/alp_project.py
            --input  ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --output ${_alp_generated}
            --emit zephyr-conf --core m55_hp
    RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "alp_project.py failed (rc=${rc})")
endif()
set(OVERLAY_CONFIG ${_alp_generated})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(iot_dashboard LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    src/dashboard_ui.c
)
