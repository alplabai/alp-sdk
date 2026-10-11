# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alp Lab AB
#
# ctest driver for alp_test_static_link_pkgconfig (#2829).
#
# Proves a non-CMake static link keeps the second backends: it asks the
# generated alp-sdk.pc for `--static --libs` / `--cflags` (libdir pointed at
# the build tree's archive), links the probe with the plain C compiler -- no
# alp::sdk INTERFACE options involved -- and then runs
# check-static-backends.cmake on the result.
#
# Usage: cmake -DPKG_CONFIG=<pkg-config> -DPC_DIR=<dir with alp-sdk.pc>
#        -DLIB=<libalp_sdk.a> -DINCLUDE_DIR=<include> -DCC=<cc> -DNM=<nm>
#        -DSRC=<probe.c> -DBIN=<output> -P <this file>

cmake_minimum_required(VERSION 3.20)

foreach(_v PKG_CONFIG PC_DIR LIB INCLUDE_DIR CC NM SRC BIN)
    if(NOT ${_v})
        message(FATAL_ERROR "check-static-pkgconfig: ${_v} is required")
    endif()
endforeach()

get_filename_component(_libdir "${LIB}" DIRECTORY)
set(ENV{PKG_CONFIG_PATH} "${PC_DIR}")

function(_alp_pc out_var)
    execute_process(COMMAND "${PKG_CONFIG}" --define-variable=libdir=${_libdir}
            --define-variable=includedir=${INCLUDE_DIR} ${ARGN} alp-sdk
        OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "check-static-pkgconfig: pkg-config ${ARGN} failed: ${_err}")
    endif()
    separate_arguments(_args UNIX_COMMAND "${_out}")
    set(${out_var} "${_args}" PARENT_SCOPE)
endfunction()

_alp_pc(_cflags --cflags)
_alp_pc(_libs --static --libs)

# Source first, flags after: the --undefined options must precede -lalp_sdk,
# which is exactly the order alp-sdk.pc emits.
execute_process(COMMAND "${CC}" "${SRC}" ${_cflags} ${_libs} -o "${BIN}"
    RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "check-static-pkgconfig: link via alp-sdk.pc failed:\n${_err}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -DNM=${NM} -DLIB=${LIB} -DBIN=${BIN}
        -P "${CMAKE_CURRENT_LIST_DIR}/check-static-backends.cmake"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _o ERROR_VARIABLE _e)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "non-CMake link via alp-sdk.pc dropped backends:\n${_e}")
endif()
message(STATUS "${_o}")
