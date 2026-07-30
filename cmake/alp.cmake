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
# be called BEFORE `find_package(Zephyr ...)`.  The reason is quoted
# verbatim from the examples this module replaces, because it is a
# non-obvious fact those files carried and there is nowhere else left to
# record it:
#
#   "`rsource` is NOT valid in .conf files (it is a Kconfig-source
#   directive only), which is why prj.conf can't pull the generated
#   fragment in itself."  So `EXTRA_CONF_FILE` is the only wiring
#   available, and Zephyr reads it during `find_package(Zephyr)` --
#   appending to it afterwards is a silent no-op, not an error.
#
# A caller that reorders these configures a build with NO
# board.yaml-derived `CONFIG_*` at all and no diagnostic anywhere.
#
# A second fact, from the same examples, is why the fragment lands in
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
# Emitter resolution.
#
# `tan` is this module's ONLY emitter -- required, not preferred.  There
# is no second code path behind it, so a missing or too-old `tan` is a
# hard configure failure with an install command in it, not a silent
# switch to an in-tree Python script that CI then has to prove twice.
#
# `--output` is the one capability probed for, and it doubles as the
# version floor.  `tan generate` otherwise hardcodes its output to
# `<project>/build/generated/<name>` and CMAKE_BINARY_DIR is NOT the
# project directory (an in-tree `west build examples/<...>` puts it under
# the repo root), so a `tan` without `--output` would emit the fragment
# somewhere this build never reads.
#
# One probe is enough, and a `--target` probe would be dead code: every
# released `tan` up to and including v0.4.1 already accepts `generate
# --target/--core/--board-yaml/--sdk-root` and NONE of them accepts
# `--output` (verified by running `tan generate --help` on the v0.4.1
# musl binary), so `--output` alone rejects exactly the too-old builds --
# including the one docs/cli.md's `install.sh` one-liner still installs.
# Probing the capability rather than parsing `tan --version` is also what
# lets a newer `tan` work here with no edit to this file.
# ----------------------------------------------------------------------
find_program(ALP_SDK_TAN_PROGRAM NAMES tan)
set(_alp_sdk_tan_help "")
if(ALP_SDK_TAN_PROGRAM)
    execute_process(
        COMMAND ${ALP_SDK_TAN_PROGRAM} generate --help
        RESULT_VARIABLE _alp_sdk_tan_rv
        OUTPUT_VARIABLE _alp_sdk_tan_stdout
        ERROR_VARIABLE  _alp_sdk_tan_stderr
    )
    if(_alp_sdk_tan_rv EQUAL 0)
        set(_alp_sdk_tan_help "${_alp_sdk_tan_stdout}${_alp_sdk_tan_stderr}")
    endif()
endif()
if(NOT "${_alp_sdk_tan_help}" MATCHES "--output")
    if(ALP_SDK_TAN_PROGRAM)
        set(_alp_sdk_tan_why
            "the `tan` at ${ALP_SDK_TAN_PROGRAM} is too old -- its `generate` \
has no `--output`")
    else()
        set(_alp_sdk_tan_why "no `tan` was found on PATH")
    endif()
    # find_program() writes its NOTFOUND into CMakeCache.txt even though
    # the configure aborts immediately below, so without this a customer
    # who then installs `tan` and re-runs the build in the SAME build tree
    # would keep getting this error from the cache.  Drop the negative
    # result along with the message.
    unset(ALP_SDK_TAN_PROGRAM CACHE)
    message(FATAL_ERROR
        "alp-sdk: ${_alp_sdk_tan_why}.\n"
        "  `tan` renders this project's board.yaml into the generated Kconfig\n"
        "  fragment (and IPC header) this build reads; alp-sdk ships no other\n"
        "  emitter, so CMake cannot continue without it.\n"
        "  Needed: a `tan` whose `generate` accepts `--output` -- tan 0.5.0-dev\n"
        "  or newer.  Released v0.4.1 and earlier do NOT, so the `install.sh`\n"
        "  one-liner in docs/cli.md is not sufficient yet.\n"
        "  Install the exact build this SDK's CI is pinned to (needs Python\n"
        "  3.12 or newer):\n"
        "    pip install \"git+https://github.com/alplabai/tan-cli@4ec44171491a8ee0a2b1dcf45b45b7757fdead0b#subdirectory=python\"\n"
        "  That commit pin, and the procedure for bumping it, is documented in\n"
        "  .github/actions/install-tan/action.yml -- keep the two in step.")
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

    set(_alp_sdk_cmd
        ${ALP_SDK_TAN_PROGRAM} generate
        --target ${mode}
        --board-yaml ${board_yaml}
        --sdk-root ${ALP_SDK_ROOT}
        --output ${output}
        --non-interactive --quiet)
    if(core)
        list(APPEND _alp_sdk_cmd --core ${core})
    endif()

    message(STATUS "alp-sdk: --emit ${mode} via tan (${ALP_SDK_TAN_PROGRAM})")
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
