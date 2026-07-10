# SPDX-License-Identifier: Apache-2.0
#
# **[UNTESTED]**: does not build in this workspace -- see README.md
# "Why this doesn't build". This CMakeLists.txt is still wired the
# way a working build needs it, so it's a correct template.

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
project(tinygsm_modem_at LANGUAGES C CXX)

target_sources(app PRIVATE src/main.cpp)

# TinyGSM has no zephyr/module.yml (west.yml `path:
# modules/lib/tinygsm`, `extras-tier1` group -- see west.yml), so
# unlike a real Zephyr module its include directory isn't added
# automatically; wire it in explicitly, same as minimp3-decode's
# vendored header.
target_include_directories(app PRIVATE ${ZEPHYR_BASE}/../modules/lib/tinygsm/src)

# Our own small Arduino-Stream shim (Print.h / Stream.h / Printable.h
# -- see src/ArduinoCompat/README.md) -- wins over the module's
# ArduinoCompat/Client.h's quote-includes of the same filenames since
# our directory is on the include path.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/ArduinoCompat)

# TINY_GSM_MODEM_SIM800 selects the SIMCOM SIM800 command set --
# matches this example's canned "SIMCOM" AT+CGMI transcript.
# ARDUINO_DASH is TinyGSM's own opt-in switch (TinyGsmCommon.h) for
# non-Arduino platforms: it swaps the Arduino-core `<Client.h>`
# `#include` for the module's bundled `<ArduinoCompat/Client.h>`,
# which is what our shim targets.
target_compile_definitions(app PRIVATE TINY_GSM_MODEM_SIM800 ARDUINO_DASH)
