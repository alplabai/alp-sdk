# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alp Lab AB
#
# ctest driver for alp_test_static_link_backends (#2790): `nm` the static
# test binary and require the registry entry `_alp_be_<class>_<name>` of every
# backend alp_sdk_force_static_backends() forced.  A missing entry means a
# static libalp_sdk.a link dropped that backend and an app would silently get
# the stub instead.
#
# Usage: cmake -DNM=<nm> -DBIN=<binary> -DSYMS=<sym|sym|...> -P <this file>

if(NOT NM OR NOT BIN OR NOT SYMS)
    message(FATAL_ERROR "check_static_backends: NM, BIN and SYMS are required")
endif()

execute_process(COMMAND "${NM}" "${BIN}"
    OUTPUT_VARIABLE _nm_out
    RESULT_VARIABLE _nm_rc)
if(NOT _nm_rc EQUAL 0)
    message(FATAL_ERROR "check_static_backends: ${NM} ${BIN} failed (${_nm_rc})")
endif()

string(REPLACE "|" ";" _syms "${SYMS}")
set(_missing "")
foreach(_sym IN LISTS _syms)
    if(NOT _nm_out MATCHES "[ \t]${_sym}\n")
        list(APPEND _missing "${_sym}")
    endif()
endforeach()

list(LENGTH _syms _n)
if(_missing)
    list(JOIN _missing "\n  " _list)
    message(FATAL_ERROR
        "static libalp_sdk.a link dropped these backends:\n  ${_list}")
endif()
message(STATUS "all ${_n} forced backends present in ${BIN}")
