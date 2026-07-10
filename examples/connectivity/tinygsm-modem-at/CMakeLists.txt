# SPDX-License-Identifier: Apache-2.0
#
# Builds + runs on native_sim.  The example drives the modem AT
# exchange directly over a mock Stream (src/mock_at_stream.h) rather
# than linking libtinygsm, whose AT engine needs Arduino's full
# `String` class -- see README.md "Why the real library isn't linked"
# and the commented real-TinyGsm wiring at the end of this file.

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

# The mock Stream (src/mock_at_stream.h) derives from our small
# Arduino-Stream shim (src/ArduinoCompat/ -- Print.h / Stream.h /
# Printable.h); put that dir on the include path.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/ArduinoCompat)

# Real TinyGSM (ARM/Arduino targets, once you supply a full Arduino
# `String`): fetch the module with
# `west update --group-filter +extras-tier1`, then link it -- it ships
# no zephyr/module.yml, so name its src/ dir explicitly and select the
# modem family + non-Arduino switch:
#
#   target_include_directories(app PRIVATE ${ZEPHYR_TINYGSM_MODULE_DIR}/src)
#   target_compile_definitions(app PRIVATE TINY_GSM_MODEM_SIM800 ARDUINO_DASH)
#
# and switch src/main.cpp to the `TinyGsm modem(stream)` sequence shown
# in its banner.
