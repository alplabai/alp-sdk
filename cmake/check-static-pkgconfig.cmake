# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alp Lab AB
#
# ctest driver for alp_test_static_link_pkgconfig (#2829).
#
# Proves a non-CMake static link keeps the second backends, end to end the way
# a Yocto recipe would consume the SDK:
#
#   1. configure + build + `cmake --install` a static ALP_OS=yocto libalp_sdk
#      into a scratch prefix with a MULTIARCH libdir (lib/x86_64-linux-gnu),
#      the layout `--prefix /usr` gives on Debian/Yocto;
#   2. resolve the INSTALLED alp-sdk.pc unmodified (PKG_CONFIG_PATH only, no
#      --define-variable), so a wrong prefix/libdir in the file fails here;
#   3. link the probe with the plain C compiler using plain `--cflags --libs`
#      (the package is archive-only, so its dependencies are in Libs/Requires,
#      not the .private variants) -- no alp::sdk INTERFACE options involved.
#      `--static` is deliberately not exercised: it also pulls the static
#      archives of every dependency (libjitterentropy.a, ...), which a host
#      without static libraries does not have;
#   4. run check-static-backends.cmake on the result.
#
# Usage: cmake -DSRC_DIR=<alp-sdk source> -DWORK_DIR=<scratch>
#        -DPKG_CONFIG=<pkg-config> -DCC=<cc> -DNM=<nm> -DSRC=<probe.c>
#        -P <this file>

cmake_minimum_required(VERSION 3.20)

foreach(_v SRC_DIR WORK_DIR PKG_CONFIG CC NM SRC)
    if(NOT ${_v})
        message(FATAL_ERROR "check-static-pkgconfig: ${_v} is required")
    endif()
endforeach()

function(_alp_run what)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _o ERROR_VARIABLE _e)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "check-static-pkgconfig: ${what} failed:\n${_o}\n${_e}")
    endif()
endfunction()

set(_build "${WORK_DIR}/build")
set(_prefix "${WORK_DIR}/prefix")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

_alp_run("configure" "${CMAKE_COMMAND}" -S "${SRC_DIR}" -B "${_build}"
    -DALP_OS=yocto -DALP_BUILD_TESTS=OFF -DCMAKE_C_COMPILER=${CC}
    -DCMAKE_INSTALL_LIBDIR=lib/x86_64-linux-gnu)
_alp_run("build" "${CMAKE_COMMAND}" --build "${_build}" --target alp_sdk)
_alp_run("install" "${CMAKE_COMMAND}" --install "${_build}" --prefix "${_prefix}")

set(_pcdir "${_prefix}/lib/x86_64-linux-gnu/pkgconfig")
if(NOT EXISTS "${_pcdir}/alp-sdk.pc")
    message(FATAL_ERROR "check-static-pkgconfig: ${_pcdir}/alp-sdk.pc not installed")
endif()
set(ENV{PKG_CONFIG_PATH} "${_pcdir}")
set(_lib "${_prefix}/lib/x86_64-linux-gnu/libalp_sdk.a")

function(_alp_pc out_var)
    execute_process(COMMAND "${PKG_CONFIG}" ${ARGN} alp-sdk
        OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "check-static-pkgconfig: pkg-config ${ARGN} failed: ${_err}")
    endif()
    separate_arguments(_args UNIX_COMMAND "${_out}")
    set(${out_var} "${_args}" PARENT_SCOPE)
endfunction()

_alp_pc(_cflags --cflags)
_alp_pc(_libs --libs)
set(_bin "${WORK_DIR}/probe")
# Source first, flags after: the --undefined options must precede
# -lalp_sdk, which is exactly the order alp-sdk.pc emits.
_alp_run("link via alp-sdk.pc" "${CC}" "${SRC}" ${_cflags} ${_libs} -o "${_bin}")
_alp_run("backend check" "${CMAKE_COMMAND}" -DNM=${NM} -DLIB=${_lib} -DBIN=${_bin}
    -P "${CMAKE_CURRENT_LIST_DIR}/check-static-backends.cmake")
message(STATUS "pkg-config: all non-anchor backends present")
