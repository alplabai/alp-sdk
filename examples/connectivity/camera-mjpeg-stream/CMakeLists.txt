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
            --emit zephyr-conf --core m55_he
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

# Layer generated CONFIG_* over prj.conf via EXTRA_CONF_FILE.
list(APPEND EXTRA_CONF_FILE ${_alp_generated})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(camera_mjpeg_stream LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    src/mjpeg_http.c
)

# Interim Ethernet PHY bring-up (MDIO refclk mode + PWRDWN/RESET GPIOs) --
# AEN-only, and only needed until board generation grows Ethernet routing
# of its own (no portable alp-sdk PHY surface exists yet).  Kept out of
# the link on every other target, including native_sim.
if(BOARD MATCHES "^alp_e1m_aen")
    target_sources(app PRIVATE src/aen_eth_phy.c)
endif()

# CI selftest -- native_sim only (CONFIG_NET_LOOPBACK gives it 127.0.0.1
# to loop back over); see src/selftest.c's file header.
if(BOARD MATCHES "native_sim")
    target_sources(app PRIVATE src/selftest.c)
endif()
