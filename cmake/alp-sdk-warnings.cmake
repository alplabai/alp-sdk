# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alp Lab AB
#
# Alp-maintainer strict-warning profile (issue #634).
#
# `alp_sdk_warnings` is an INTERFACE target carrying the compiler-warning
# flags a strict, warning-clean first-party C build wants.  It is applied
# PER-SOURCE-FILE via `alp_sdk_apply_strict_warnings()` below -- never with
# a blanket `target_link_libraries(<tgt> PRIVATE alp_sdk_warnings)` -- to
# FIRST-PARTY Alp sources only (today: everything under this repo's own
# `src/` tree, reached through the `alp_sdk` and `alp_chips` targets).
# `vendors/**` sources (e.g. `vendors/alif/*.c`, pulled into the SAME
# `alp_sdk` target via `target_sources()`) and `zephyr/**` never get these
# flags: a target-wide application would silently strict-warn (and, with
# ALP_SDK_WERROR, hard-error) that vendor-shaped code too, breaking the
# "vendored/upstream code never gets -Werror" rule this profile exists to
# uphold (ADR 0017 tier discipline).  It is also never linked through
# `alp::sdk` to a downstream consumer.
#
# Two independent opt-ins, both OFF by default so this is fully inert
# for every existing consumer and CI job that doesn't ask for it:
#
#   ALP_SDK_STRICT_WARNINGS  -- turns the flag set on at all.
#   ALP_SDK_WERROR           -- additionally turns every one of those
#                               warnings into a hard error.  Meaningless
#                               without ALP_SDK_STRICT_WARNINGS=ON.
#
# The flag list mirrors the strict GCC 13.3 pass from issue #634 verbatim,
# with a Clang-mapped equivalent (Clang has no `-Wcast-align=strict` --
# plain `-Wcast-align` already covers the stricter check on Clang).

option(ALP_SDK_STRICT_WARNINGS
    "Build first-party alp-sdk C targets (libalp_sdk, libalp_chips) with the \
Alp-maintainer strict warning profile (-Wall -Wextra + the #634 flag set). \
Never applied to vendors/**, zephyr/**, or any vendored/upstream sources." OFF)

option(ALP_SDK_WERROR
    "Promote the ALP_SDK_STRICT_WARNINGS profile to -Werror.  No effect \
unless ALP_SDK_STRICT_WARNINGS is also ON." OFF)

add_library(alp_sdk_warnings INTERFACE)

if(ALP_SDK_STRICT_WARNINGS)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU")
        target_compile_options(alp_sdk_warnings INTERFACE
            -Wall
            -Wextra
            -Wpedantic
            -Wformat=2
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wcast-align=strict
            -Wstrict-prototypes
            -Wmissing-prototypes
            -Wdouble-promotion
            -Wundef
        )
    elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(alp_sdk_warnings INTERFACE
            -Wall
            -Wextra
            -Wpedantic
            -Wformat=2
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wcast-align
            -Wstrict-prototypes
            -Wmissing-prototypes
            -Wdouble-promotion
            -Wundef
        )
    else()
        message(WARNING
            "alp-sdk: ALP_SDK_STRICT_WARNINGS has no flag mapping for "
            "CMAKE_C_COMPILER_ID='${CMAKE_C_COMPILER_ID}' (only GNU/Clang are "
            "mapped) -- building without the strict profile.")
    endif()

    if(ALP_SDK_WERROR)
        target_compile_options(alp_sdk_warnings INTERFACE -Werror)
    endif()
endif()

# alp_sdk_apply_strict_warnings(<target> <first_party_prefix>)
#
# Walks <target>'s already-registered SOURCES and applies the
# alp_sdk_warnings flag set (via per-source COMPILE_OPTIONS) to every one
# whose path starts with <first_party_prefix> -- e.g. "${CMAKE_CURRENT_
# SOURCE_DIR}/src/" -- leaving any source added from elsewhere (vendors/**,
# zephyr/**) untouched.  A no-op when ALP_SDK_STRICT_WARNINGS is OFF (the
# default).  Call this AFTER every target_sources()/add_subdirectory() that
# populates <target>, so the SOURCES property is complete.
function(alp_sdk_apply_strict_warnings target first_party_prefix)
    if(NOT ALP_SDK_STRICT_WARNINGS)
        return()
    endif()
    get_target_property(_alp_sdk_warn_opts alp_sdk_warnings INTERFACE_COMPILE_OPTIONS)
    if(NOT _alp_sdk_warn_opts)
        return()
    endif()
    get_target_property(_alp_sdk_apply_srcs ${target} SOURCES)
    if(NOT _alp_sdk_apply_srcs)
        return()
    endif()
    foreach(_alp_sdk_apply_src IN LISTS _alp_sdk_apply_srcs)
        string(FIND "${_alp_sdk_apply_src}" "${first_party_prefix}" _alp_sdk_apply_pos)
        if(_alp_sdk_apply_pos EQUAL 0)
            set_source_files_properties(${_alp_sdk_apply_src} PROPERTIES
                COMPILE_OPTIONS "${_alp_sdk_warn_opts}")
        endif()
    endforeach()
endfunction()
