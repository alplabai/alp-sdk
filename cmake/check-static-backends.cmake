# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alp Lab AB
#
# ctest driver for alp_test_static_link_backends (#2790).
#
# The expected set comes from the static archive itself, NOT from the
# source scan in alp-sdk-static-backends.cmake, so a registration that scan
# misses is caught here instead of being skipped by both:
#
#   1. `nm -A` the archive and collect, per member, its registry entries
#      (`_alp_be_<class>_<name>`) and whether it defines a dispatcher anchor
#      (`_alp_backend_anchor_<class>`).
#   2. Members that define an anchor are the catch-alls the dispatcher pulls
#      only when the class is used; drop them.
#   3. Every registry entry left must be present in the linked binary.
#      A missing one means a static link dropped that backend and an app
#      would silently get the stub.
#
# Usage: cmake -DNM=<nm> -DLIB=<libalp_sdk.a> -DBIN=<binary> -P <this file>

# -P scripts start with every policy unset; IN_LIST needs CMP0057.
cmake_minimum_required(VERSION 3.20)

if(NOT NM OR NOT LIB OR NOT BIN)
    message(FATAL_ERROR "check-static-backends: NM, LIB and BIN are required")
endif()

function(_alp_nm out_var)
    execute_process(COMMAND "${NM}" ${ARGN}
        OUTPUT_VARIABLE _out
        ERROR_QUIET
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "check-static-backends: ${NM} ${ARGN} failed (${_rc})")
    endif()
    set(${out_var} "${_out}" PARENT_SCOPE)
endfunction()

# GNU nm -A on an archive prints "<archive>:<member>:<addr> <type> <symbol>".
_alp_nm(_lib_out -A "${LIB}")
string(REPLACE "\n" ";" _lines "${_lib_out}")
set(_anchored_members "")
set(_entries "")
foreach(_line IN LISTS _lines)
    if(_line MATCHES "^.*:([^:]+):[0-9a-fA-F ]* ([A-Za-z]) (_alp_[A-Za-z0-9_]+)$")
        set(_member "${CMAKE_MATCH_1}")
        set(_type "${CMAKE_MATCH_2}")
        set(_sym "${CMAKE_MATCH_3}")
        if(_type STREQUAL "U")
            continue()
        endif()
        if(_sym MATCHES "^_alp_backend_anchor_" AND NOT _sym MATCHES "^_alp_backend_anchor_(ref|decl)_")
            list(APPEND _anchored_members "${_member}")
        elseif(_sym MATCHES "^_alp_be_")
            list(APPEND _entries "${_member}|${_sym}")
        endif()
    endif()
endforeach()

set(_expected "")
foreach(_entry IN LISTS _entries)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _member)
    list(GET _parts 1 _sym)
    if(NOT _member IN_LIST _anchored_members)
        list(APPEND _expected "${_sym}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _expected)
list(LENGTH _expected _n)
if(_n EQUAL 0)
    message(FATAL_ERROR "check-static-backends: no non-anchor backends found in ${LIB}")
endif()

_alp_nm(_bin_out "${BIN}")
set(_missing "")
foreach(_sym IN LISTS _expected)
    if(NOT _bin_out MATCHES "[ \t]${_sym}\n")
        list(APPEND _missing "${_sym}")
    endif()
endforeach()

if(_missing)
    list(JOIN _missing "\n  " _list)
    message(FATAL_ERROR
        "static libalp_sdk.a link dropped these backends:\n  ${_list}")
endif()
message(STATUS "all ${_n} non-anchor backends of ${LIB} present in ${BIN}")
