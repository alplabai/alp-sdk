# SPDX-License-Identifier: Apache-2.0
#
# libhelix-decode builds the PORTABLE half of the example (MP3 frame
# sync + header parse) on any target, including native_sim.  The real
# Helix fixed-point PCM decode compiles only on a supported ARM target
# (real/assembly.h #errors on generic x86-64) and links the
# west-fetched module (extras-tier1 group, RCSL licence); see
# src/main.c's banner + README.md for the real MP3Decode() wiring.

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
project(libhelix_decode LANGUAGES C)

target_sources(app PRIVATE src/main.c)

# Real Helix PCM decode (ARM targets only): once you build for an
# Ensemble E8 SoM and `west update --group-filter +extras-tier1`
# fetches modules/lib/libhelix (ultraembedded/libhelix-mp3), compile
# the decoder core and add its public headers, e.g.:
#
#   target_include_directories(app PRIVATE ${ZEPHYR_LIBHELIX_MODULE_DIR}/pub)
#   target_sources(app PRIVATE
#       ${ZEPHYR_LIBHELIX_MODULE_DIR}/mp3dec.c
#       ${ZEPHYR_LIBHELIX_MODULE_DIR}/mp3tabs.c
#       ${ZEPHYR_LIBHELIX_MODULE_DIR}/real/*.c)
#
# then switch src/main.c to the MP3Decode() path shown in its banner.
# The module ships no zephyr/module.yml, so west won't add its include
# path automatically -- name it explicitly as above.
