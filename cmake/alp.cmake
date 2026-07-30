# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alp Lab AB
#
# The shared `board.yaml` -> generated-artefact bridge every Zephyr
# example (and every project scaffolded from one) calls at CMake
# configure time.
#
# Before this module the same ~25 lines were copy-pasted into 96 example
# `CMakeLists.txt` files in six near-identical shapes.  Twenty of those
# hardcoded a `${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/
# alp_project.py` path -- no `ALP_SDK_ROOT` override was possible at all
# -- and captured no `ERROR_VARIABLE`, so a board.yaml load failure threw
# the loader's own diagnostic away and reported a bare `rc=1`.
#
# ======================================================================
# ORDER IS LOAD-BEARING.  Do not "simplify" it away.
# ======================================================================
#
# `alp_sdk_zephyr_conf()` and `alp_sdk_ipc_contract_header()` MUST both
# be called BEFORE `find_package(Zephyr ...)`, for two independent
# reasons.  Both are quoted verbatim from the examples this module
# replaces, because they are the only two non-obvious facts those files
# carried and there is nowhere else left to record them:
#
#   1. "find_package(Python3 ...) MUST run before find_package(Zephyr
#      ...) because Zephyr's CMake machinery pins a Python interpreter
#      the moment it imports."
#
#   2. "`rsource` is NOT valid in .conf files (it is a Kconfig-source
#      directive only), which is why prj.conf can't pull the generated
#      fragment in itself."  So `EXTRA_CONF_FILE` is the only wiring
#      available, and Zephyr reads it during `find_package(Zephyr)` --
#      appending to it afterwards is a silent no-op, not an error.
#
# A caller that reorders these configures a build with NO
# board.yaml-derived `CONFIG_*` at all and no diagnostic anywhere.
#
# A third fact, from the same examples, is why the fragment lands in
# `generated/` and never at the build-dir root: "Zephyr's kconfig.cmake
# greps `${APPLICATION_BINARY_DIR}/*.conf` at the END of the
# merge-config-files list (cmake/modules/kconfig.cmake line 303 in
# v4.4.0), so any .conf file dropped in the build-root wins over every
# `EXTRA_CONF_FILE=...` overlay" -- including a per-example
# `native_sim.conf`.  Eleven of the 96 examples this module replaced wrote
# `${CMAKE_CURRENT_BINARY_DIR}/alp.conf`, i.e. straight into that glob,
# and silently beat their own overlays.  One path, one level deeper, fixes
# all of them.
#
# ======================================================================
# ALP_SDK_ROOT
# ======================================================================
#
# `ALP_SDK_ROOT` must be set before `include()`ing this file -- it is how
# this module finds its own emitter.  Every in-tree example resolves it
# from the environment first and only then falls back to walking up to
# the SDK checkout, because that walk is correct ONLY for the in-tree
# example: a customer who copies the example OUT of the SDK tree has no
# relative path to guess from.  `--emit scaffold` therefore rewrites the
# in-tree fallback into a hard requirement -- see
# `scripts/alp_template.py::_scaffold_cmakelists`, which fails loudly if
# it ever meets a shape it does not recognise.

include_guard(GLOBAL)

# ----------------------------------------------------------------------
# Emitter resolution (once per build tree, cached).
#
# `tan` is the SDK's own front door and is preferred over shelling
# `scripts/alp_project.py` -- but only when the `tan` on PATH can write
# to an explicit path.  `tan generate` hardcodes its output to
# `<project>/build/generated/<name>` and CMAKE_BINARY_DIR is NOT the
# project directory (an in-tree `west build examples/<...>` puts it under
# the repo root), so a `tan` without `--output` would emit the fragment
# somewhere this build never reads.  Probe for that one flag rather than
# assume it: an older `tan` keeps working, a newer one is picked up with
# no further change here.
#
# Both modes are served by `tan generate --target` as of tan v0.5.0; the
# fallback below stays for a customer whose `tan` predates it.
# ----------------------------------------------------------------------
if(NOT DEFINED ALP_SDK_TAN_EMITTER)
    set(ALP_SDK_TAN_EMITTER "" CACHE INTERNAL
        "Absolute path to a `tan` that can serve alp_sdk_zephyr_conf(), or \
empty when the alp_project.py fallback is in use.")
    find_program(ALP_SDK_TAN_PROGRAM NAMES tan)
    if(ALP_SDK_TAN_PROGRAM)
        execute_process(
            COMMAND ${ALP_SDK_TAN_PROGRAM} generate --help
            RESULT_VARIABLE _alp_sdk_tan_rv
            OUTPUT_VARIABLE _alp_sdk_tan_stdout
            ERROR_VARIABLE  _alp_sdk_tan_stderr
        )
        if(_alp_sdk_tan_rv EQUAL 0
           AND "${_alp_sdk_tan_stdout}${_alp_sdk_tan_stderr}" MATCHES "--output")
            set(ALP_SDK_TAN_EMITTER "${ALP_SDK_TAN_PROGRAM}" CACHE INTERNAL "" FORCE)
        endif()
    endif()
endif()

# _alp_sdk_emit(<mode> <core> <board_yaml> <output>)
#
# Run one board.yaml -> artefact emit, or fail with the emitter's own
# stderr.  `<core>` may be empty for a mode that is not core-scoped.
# Internal: callers use the two documented entry points below.
function(_alp_sdk_emit mode core board_yaml output)
    if(NOT EXISTS "${board_yaml}")
        message(FATAL_ERROR
            "alp-sdk: no board.yaml at ${board_yaml} -- alp_sdk_zephyr_conf() "
            "renders THIS project's board.yaml, so the file must exist before "
            "CMake configures.")
    endif()

    if(ALP_SDK_TAN_EMITTER)
        set(_alp_sdk_cmd
            ${ALP_SDK_TAN_EMITTER} generate
            --target ${mode}
            --board-yaml ${board_yaml}
            --sdk-root ${ALP_SDK_ROOT}
            --output ${output}
            --non-interactive --quiet)
        if(core)
            list(APPEND _alp_sdk_cmd --core ${core})
        endif()
        set(_alp_sdk_via "tan (${ALP_SDK_TAN_EMITTER})")
    else()
        # The fallback, logged rather than silent: a customer mid-upgrade
        # with no `tan` on PATH (or one predating `--output`) must still
        # be able to configure this project.
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(_alp_sdk_cmd
            ${Python3_EXECUTABLE} ${ALP_SDK_ROOT}/scripts/alp_project.py
            --input ${board_yaml}
            --emit ${mode}
            --output ${output})
        if(core)
            list(APPEND _alp_sdk_cmd --core ${core})
        endif()
        set(_alp_sdk_via "scripts/alp_project.py (no `tan` with --output on PATH)")
    endif()

    message(STATUS "alp-sdk: --emit ${mode} via ${_alp_sdk_via}")
    execute_process(
        COMMAND ${_alp_sdk_cmd}
        RESULT_VARIABLE _alp_sdk_rv
        OUTPUT_VARIABLE _alp_sdk_stdout
        ERROR_VARIABLE  _alp_sdk_stderr
    )
    if(NOT _alp_sdk_rv EQUAL 0)
        message(FATAL_ERROR
            "alp-sdk: --emit ${mode} failed (rv=${_alp_sdk_rv}); "
            "check ${board_yaml}.\n"
            "stderr: ${_alp_sdk_stderr}")
    endif()
endfunction()

# alp_sdk_zephyr_conf(<core_id> [BOARD_YAML <path>])
#
# Render `<core_id>`'s view of this project's `board.yaml` into a Kconfig
# fragment and append it to `EXTRA_CONF_FILE` so Zephyr layers it over
# `prj.conf`.  `BOARD_YAML` defaults to `${CMAKE_CURRENT_SOURCE_DIR}/
# board.yaml`; a per-core subdirectory of a multicore project passes its
# parent's (`.../../board.yaml`).
#
# The fragment lands at `${CMAKE_BINARY_DIR}/generated/alp.conf` -- the
# path `metadata/emit-registry-v1.json`, the template catalog and `tan`'s
# own `output_relative_path` all name, and the one
# `scripts/check_zephyr_conf_parity.py` pins byte-for-byte against the
# planner's build-plan `configArtefacts`.
#
# MUST be called before `find_package(Zephyr ...)` -- see the header.
function(alp_sdk_zephyr_conf core_id)
    cmake_parse_arguments(_alp_sdk_arg "" "BOARD_YAML" "" ${ARGN})
    if(NOT core_id)
        message(FATAL_ERROR
            "alp-sdk: alp_sdk_zephyr_conf() needs the one core id this "
            "CMakeLists.txt builds. An unscoped, cross-core Kconfig sum is "
            "the leak ADR-0020's addendum retired.")
    endif()
    if(_alp_sdk_arg_BOARD_YAML)
        set(_alp_sdk_board "${_alp_sdk_arg_BOARD_YAML}")
    else()
        set(_alp_sdk_board "${CMAKE_CURRENT_SOURCE_DIR}/board.yaml")
    endif()

    set(_alp_sdk_conf "${CMAKE_BINARY_DIR}/generated/alp.conf")
    _alp_sdk_emit(zephyr-conf "${core_id}" "${_alp_sdk_board}" "${_alp_sdk_conf}")

    # `list(APPEND EXTRA_CONF_FILE ...)` in the caller's scope -- a
    # function() body has its own variable scope, so the append has to be
    # handed back explicitly or Zephyr never sees the fragment.
    set(_alp_sdk_extra ${EXTRA_CONF_FILE})
    list(APPEND _alp_sdk_extra "${_alp_sdk_conf}")
    set(EXTRA_CONF_FILE "${_alp_sdk_extra}" PARENT_SCOPE)
endfunction()

# alp_sdk_ipc_contract_header([BOARD_YAML <path>])
#
# Render `<alp/system_ipc.h>` from this project's `board.yaml` `ipc:`
# block into `${CMAKE_BINARY_DIR}/generated/alp/system_ipc.h`.  Not
# core-scoped: the contract is the cross-core agreement itself.
#
# Deliberately separate from `alp_sdk_zephyr_conf()` rather than a mode
# argument to it: the two wire up differently and at different times.
# This one only WRITES the header -- putting it on the include path needs
# `zephyr_include_directories(${CMAKE_BINARY_DIR}/generated)`, which does
# not exist until `find_package(Zephyr)` has run, so it stays at the call
# site AFTER that call while this emit stays before it.
function(alp_sdk_ipc_contract_header)
    cmake_parse_arguments(_alp_sdk_arg "" "BOARD_YAML" "" ${ARGN})
    if(_alp_sdk_arg_BOARD_YAML)
        set(_alp_sdk_board "${_alp_sdk_arg_BOARD_YAML}")
    else()
        set(_alp_sdk_board "${CMAKE_CURRENT_SOURCE_DIR}/board.yaml")
    endif()
    _alp_sdk_emit(ipc-contract-h "" "${_alp_sdk_board}"
        "${CMAKE_BINARY_DIR}/generated/alp/system_ipc.h")
endfunction()
