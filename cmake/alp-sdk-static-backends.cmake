# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alp Lab AB
#
# Keep every non-anchor backend in a STATIC libalp_sdk.a link (issue #2790).
#
# A static-archive link only pulls the members something references.  Each
# class dispatcher references its catch-all backend through
# ALP_BACKEND_ANCHOR (see <alp/backend.h>), but a second backend that sits in
# its own TU -- the Linux src/backends/<class>/yocto_drv.c, gpu2d's
# yocto_gles.c, rpc's yocto_uio_drv.c -- is pure registry data that nothing
# names.  The linker drops it and the app silently gets the stub or the
# software fallback, with no build error.
#
# ALP_BACKEND_REGISTER exports `_alp_backend_force_<class>_<name>` on a
# plain-CMake build (ALP_BACKEND_FORCE_DEFINE).  alp_sdk_force_static_backends()
# runs once all sources are on the target, reads every C source for its
# column-0 ALP_BACKEND_REGISTER(<class>, <name> calls, and adds
# `--undefined=_alp_backend_force_<class>_<name>` to the target's INTERFACE
# link options, so every consumer's link pulls those members.  Deriving the
# list from the sources means a new backend needs no CMake line of its own.
#
# Skipped:
#   - sources that define ALP_BACKEND_ANCHOR_DEFINE: the dispatcher already
#     pulls those catch-alls, and forcing them would drag unused classes in;
#   - registrations wrapped in another macro (indented, e.g. the cc3501e
#     ble/wifi backends): their <name> is a macro parameter, not a symbol;
#   - shared builds (every member is in the .so already) and non-ELF hosts
#     (Mach-O/PE linkers have no --undefined, and the alp_backends_<class>
#     section bounds are ELF-only anyway).
#
# A consumer that compiles backend sources into itself opts out by setting
# the ALP_SDK_NO_FORCED_BACKENDS target property; otherwise the forced archive
# copy and its own copy would clash.
#
# The forced "<class>:<name>" pairs are recorded on the target's
# ALP_SDK_FORCED_BACKENDS property; tests/yocto uses it to check, with nm,
# that a static test binary really carries each backend.

function(alp_sdk_force_static_backends target)
    if(ALP_SDK_BUILD_SHARED OR APPLE OR WIN32)
        return()
    endif()

    get_target_property(_srcs ${target} SOURCES)
    set(_forced "")
    foreach(_src IN LISTS _srcs)
        if(NOT _src MATCHES "\\.c$" OR NOT EXISTS "${_src}")
            continue()
        endif()
        file(READ "${_src}" _text)
        if(_text MATCHES "\nALP_BACKEND_ANCHOR_DEFINE\\(")
            continue()
        endif()
        # Prepend a newline so a call on the file's first line still
        # counts as column 0.
        string(REGEX MATCHALL
            "\nALP_BACKEND_REGISTER\\([ \t\r\n]*[a-z0-9_]+[ \t\r\n]*,[ \t\r\n]*[A-Za-z0-9_]+"
            _calls "\n${_text}")
        foreach(_call IN LISTS _calls)
            string(REGEX REPLACE
                "^\nALP_BACKEND_REGISTER\\([ \t\r\n]*([a-z0-9_]+)[ \t\r\n]*,[ \t\r\n]*([A-Za-z0-9_]+)$"
                "\\1:\\2" _pair "${_call}")
            list(APPEND _forced "${_pair}")
        endforeach()
    endforeach()

    list(REMOVE_DUPLICATES _forced)
    # A consumer that compiles its own copy of some backends (a test that
    # must select the sw_fallback, or one that builds a backend .c in) sets
    # ALP_SDK_NO_FORCED_BACKENDS on itself; $<TARGET_PROPERTY:...> in an
    # INTERFACE option reads the consuming target, so the options drop out
    # for that link only.
    set(_wanted "$<NOT:$<BOOL:$<TARGET_PROPERTY:ALP_SDK_NO_FORCED_BACKENDS>>>")
    foreach(_pair IN LISTS _forced)
        string(REPLACE ":" "_" _sym "${_pair}")
        target_link_options(${target} INTERFACE
            "$<${_wanted}:LINKER:--undefined=_alp_backend_force_${_sym}>")
    endforeach()
    set_property(TARGET ${target} PROPERTY ALP_SDK_FORCED_BACKENDS "${_forced}")
endfunction()
