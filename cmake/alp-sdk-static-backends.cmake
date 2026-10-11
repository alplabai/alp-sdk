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
#   - registrations of a class whose ALP_BACKEND_ANCHOR_DEFINE is in the same
#     source: the dispatcher already pulls those catch-alls when the class is
#     used;
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
# Cost: every static consumer now links every forced backend, whether or not
# it uses the class, and with them their runtime libraries (libasound,
# libssl/libcrypto, libmosquitto, open-amp/libmetal) when the build found
# them -- the same set a shared libalp_sdk.so already carries.
#
# The forced "<class>:<name>" pairs are recorded on the target's
# ALP_SDK_FORCED_BACKENDS property: the list a non-CMake static link must pass
# as --undefined= options itself.  tests/yocto checks the result against the
# archive, not against this list (cmake/check-static-backends.cmake).

function(alp_sdk_force_static_backends target)
    if(ALP_SDK_BUILD_SHARED OR APPLE OR WIN32)
        return()
    endif()

    get_target_property(_srcs ${target} SOURCES)
    set(_forced "")
    foreach(_src IN LISTS _srcs)
        if(NOT _src MATCHES "\\.(c|cc|cpp)$" OR NOT EXISTS "${_src}")
            continue()
        endif()
        file(READ "${_src}" _text)
        # Classes this TU anchors; only their registrations are skipped, so
        # a TU that also registers a non-anchored class still gets forced.
        set(_anchored "")
        string(REGEX MATCHALL "\nALP_BACKEND_ANCHOR_DEFINE\\([ \t\r\n]*[a-z0-9_]+"
            _anchor_calls "\n${_text}")
        foreach(_anchor IN LISTS _anchor_calls)
            string(REGEX REPLACE "^.*\\([ \t\r\n]*([a-z0-9_]+)$" "\\1" _aclass "${_anchor}")
            list(APPEND _anchored "${_aclass}")
        endforeach()
        # Prepend a newline so a call on the file's first line still
        # counts as column 0.
        string(REGEX MATCHALL
            "\nALP_BACKEND_REGISTER\\([ \t\r\n]*[a-z0-9_]+[ \t\r\n]*,[ \t\r\n]*[A-Za-z0-9_]+"
            _calls "\n${_text}")
        foreach(_call IN LISTS _calls)
            string(REGEX REPLACE
                "^\nALP_BACKEND_REGISTER\\([ \t\r\n]*([a-z0-9_]+)[ \t\r\n]*,[ \t\r\n]*([A-Za-z0-9_]+)$"
                "\\1:\\2" _pair "${_call}")
            string(REGEX REPLACE ":.*$" "" _pclass "${_pair}")
            if(_pclass IN_LIST _anchored)
                continue()
            endif()
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

# Non-CMake consumers (a Yocto recipe or Makefile doing `gcc app.c
# $(pkg-config --static --libs alp-sdk)`) never see the INTERFACE link
# options above, so a plain `-lalp_sdk` drops every forced backend and the
# app silently runs on the sw_fallback (#2829).  This writes
# <out_dir>/alp-sdk.pc whose Libs: carries the same
# `-Wl,--undefined=_alp_backend_force_<class>_<name>` list, placed BEFORE
# -lalp_sdk: the linker must already hold the undefined symbol when it scans
# the archive.  No file is written when nothing was forced (shared, Apple,
# Windows, or no second backends).  The system libraries the forced backends
# need (libasound, libssl, ...) go in Libs.private, flattened from the
# libraries alp_sdk was configured with, so `pkg-config --static --libs`
# returns a complete link line.
# Libs.private items from the target's link interface string: the
# pkg-config modules it links (PkgConfig::<PFX> -> <PFX>_LDFLAGS, set
# by pkg_check_modules) and the absolute library paths it names (zcbor,
# DEEPX, MERA, ...).  Read as text rather than walked as a list: the property
# carries $<LINK_ONLY:...> wrappers.
function(_alp_pc_private deps out_var)
    set(_out "-lm")
    if(deps MATCHES "Threads::Threads")
        list(APPEND _out "-lpthread")
    endif()
    string(REGEX MATCHALL "PkgConfig::[A-Za-z0-9_]+" _mods "${deps}")
    foreach(_m IN LISTS _mods)
        string(REPLACE "PkgConfig::" "" _pfx "${_m}")
        if(DEFINED ${_pfx}_LDFLAGS)
            list(APPEND _out ${${_pfx}_LDFLAGS})
        endif()
    endforeach()
    string(REGEX MATCHALL "/[^;>$<]+\\.(a|so)" _paths "${deps}")
    list(APPEND _out ${_paths})
    list(REMOVE_DUPLICATES _out)
    set(${out_var} "${_out}" PARENT_SCOPE)
endfunction()

function(alp_sdk_write_static_pkgconfig target out_dir)
    get_target_property(_forced ${target} ALP_SDK_FORCED_BACKENDS)
    if(NOT _forced)
        return()
    endif()
    set(ALP_SDK_PC_FORCED_FLAGS "")
    foreach(_pair IN LISTS _forced)
        string(REPLACE ":" "_" _sym "${_pair}")
        string(APPEND ALP_SDK_PC_FORCED_FLAGS
            " -Wl,--undefined=_alp_backend_force_${_sym}")
    endforeach()
    # Libraries alp_sdk itself links (PUBLIC when static): flatten the
    # imported pkg-config targets to -l/-L/path items for Libs.private.
    get_target_property(_deps ${target} INTERFACE_LINK_LIBRARIES)
    _alp_pc_private("${_deps}" _priv)
    set(ALP_SDK_PC_LIBS_PRIVATE "")
    foreach(_l IN LISTS _priv)
        string(APPEND ALP_SDK_PC_LIBS_PRIVATE " ${_l}")
    endforeach()
    set(ALP_SDK_PC_VERSION "${PROJECT_VERSION}")
    set(ALP_SDK_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
    set(ALP_SDK_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
    configure_file(${PROJECT_SOURCE_DIR}/cmake/alp-sdk.pc.in
        ${out_dir}/alp-sdk.pc @ONLY)
endfunction()
