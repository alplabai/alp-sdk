# SPDX-License-Identifier: Apache-2.0
"""Regression tests for the `os: baremetal` slice's build command,
post-build step and artifact block -- alplabai/tan-cli#550 / #551.

Before these landed, a baremetal slice:

  * emitted a SINGLE `cmake -S <app> -B <buildDir>` step, which only
    CONFIGURES. `tan build` read its exit 0 as a built slice and printed
    `1 of 1 slice(s) built` over a build tree holding `CMakeCache.txt`
    and nothing else -- no `.o`, no `.a`, no executable (tan-cli#550);
  * resolved that relative `-B` against the command's own `cwd` (the
    plan pins `cwd` to the slice's buildDir), double-nesting the tree at
    `<buildDir>/build/<core>-baremetal/` where nothing reading
    `buildDir`/`artifacts` would ever find it;
  * reported an all-null `artifacts` block, so no consumer could notice
    the missing binary either (tan-cli#550);
  * carried not one of its `-DALP_*` defines on the configure -- the
    app saw `ALP_CORE_ID`, `ALP_SOM_SKU` and `ALP_BOARD_<SLUG>` all
    empty and still reported `status: ok, rc: 0` (tan-cli#551).

The last test in this module is the end-to-end proof: it MATERIALISES
the plan's artefacts and RUNS the emitted command + postCommands with a
real cmake, then asserts a real executable exists at the path
`artifacts.outputDir` promised.

Run locally:

    python -m pytest tests/scripts/test_orchestrate_baremetal_slice.py -v
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import REPO, _write_board  # noqa: E402

from alp_orchestrate import emit_build_plan, load_board_yaml  # noqa: E402

# `preset: e1m-evk` is load-bearing: it is what makes the project resolve
# a board name, hence an `ALP_BOARD_E1M_EVK` compile guard -- the exact
# define tan-cli#551 names first.
AEN801_BAREMETAL = """
som:
  sku: E1M-AEN801
  hw_rev: r1

preset: e1m-evk

cores:
  m55_hp:
    os: baremetal
    app: ./src
"""


def _baremetal_plan(tmp_path: Path) -> tuple[Path, dict]:
    """(project dir, the m55_hp baremetal slice) for the fixture above."""
    (tmp_path / "src").mkdir(exist_ok=True)
    (tmp_path / "src" / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\nproject(demo C)\n",
        encoding="utf-8")
    path = _write_board(tmp_path, AEN801_BAREMETAL)
    project = load_board_yaml(path)
    plan = json.loads(emit_build_plan(
        project, board_yaml=path, build_root=Path("build")))
    slice_ = next(s for s in plan["slices"] if s["backend"] == "baremetal")
    return tmp_path, slice_


# ---------------------------------------------------------------------
# tan-cli#550 -- the slice must actually BUILD, into a findable tree,
# and describe what it produces.
# ---------------------------------------------------------------------


def test_baremetal_slice_carries_a_build_step(tmp_path: Path) -> None:
    """`cmake -S ... -B ...` only configures. The slice must carry a
    `cmake --build` step in `postCommands`, or an executor that runs
    `command` alone reports a green build over an empty output dir
    (tan-cli#550)."""
    _, slice_ = _baremetal_plan(tmp_path)

    assert slice_["postCommands"] == [{
        "tool": "cmake",
        "args": ["--build", "."],
        "cwd":  "build/m55_hp-baremetal",
    }]


def test_baremetal_configure_does_not_nest_the_build_tree(
        tmp_path: Path) -> None:
    """`-B` is `.`, never the buildDir path: the plan pins `cwd` to the
    slice's buildDir and cmake resolves a relative `-B` against its own
    cwd, so `-B build/m55_hp-baremetal` put the tree two levels down at
    `<buildDir>/build/m55_hp-baremetal/` -- outside everything that reads
    `buildDir`/`artifacts`."""
    _, slice_ = _baremetal_plan(tmp_path)
    args = slice_["command"]["args"]

    assert slice_["command"]["cwd"] == "build/m55_hp-baremetal"
    assert args[args.index("-B") + 1] == "."
    assert "build/m55_hp-baremetal" not in args


def test_baremetal_artifacts_describe_what_the_slice_produces(
        tmp_path: Path) -> None:
    """The artifacts block was all-null, so a consumer could not tell a
    slice that produced nothing from one that built fine (tan-cli#550).
    The two paths the configure line now FORCES are reported; the ELF's
    NAME stays unreported because the app's own CMakeLists.txt picks it
    and this emitter must never invent one."""
    _, slice_ = _baremetal_plan(tmp_path)

    assert slice_["artifacts"]["compileCommands"] == \
        "build/m55_hp-baremetal/compile_commands.json"
    assert slice_["artifacts"]["outputDir"] == \
        "build/m55_hp-baremetal/output"
    # Never guessed: no SDK-wide baremetal executable-name convention.
    for key in ("elf", "map", "bin", "sizeReport", "symbols"):
        assert slice_["artifacts"][key] is None

    args = slice_["command"]["args"]
    assert "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON" in args
    # Absolute + ${PROJECT_ROOT}-tokened (#865/#596): CMake resolves a
    # RELATIVE runtime-output dir against each target's own
    # CMAKE_CURRENT_BINARY_DIR, scattering a multi-subdirectory app's
    # outputs instead of pinning them where `outputDir` claims.
    assert ("-DCMAKE_RUNTIME_OUTPUT_DIRECTORY="
            "${PROJECT_ROOT}/build/m55_hp-baremetal/output") in args


def test_zephyr_and_yocto_slices_carry_no_post_commands(
        tmp_path: Path) -> None:
    """`west build` and `bitbake` each configure AND build in one
    invocation -- adding a second step there would build twice. Only the
    two-phase cmake backend gets `postCommands`."""
    _, _ = _baremetal_plan(tmp_path)
    path = tmp_path / "board.yaml"
    project = load_board_yaml(path)
    plan = json.loads(emit_build_plan(
        project, board_yaml=path, build_root=Path("build")))

    for slice_ in plan["slices"]:
        if slice_["backend"] != "baremetal":
            assert slice_["postCommands"] == [], slice_["coreId"]
            assert slice_["artifacts"]["outputDir"] is None, slice_["coreId"]


def test_command_less_slice_carries_no_post_commands(tmp_path: Path) -> None:
    """A baremetal slice whose command is BLOCKED (here by #865's
    unrooted-app guard: an `app:` under neither ${PROJECT_ROOT} nor
    ${SDK_ROOT}) is carried with `command: null` + a warning. There is
    nothing to build on top of a slice that was never configured, so
    `postCommands` must stay empty rather than send `cmake --build .`
    into an unconfigured directory."""
    path = _write_board(tmp_path, """
        som:
          sku: E1M-AEN801
          hw_rev: r1

        preset: e1m-evk

        cores:
          m55_hp:
            os: baremetal
            app: /nowhere-outside-both-roots
    """)
    project = load_board_yaml(path)
    plan = json.loads(emit_build_plan(
        project, board_yaml=path, build_root=Path("build")))
    slice_ = next(s for s in plan["slices"] if s["coreId"] == "m55_hp")

    assert slice_["command"] is None
    assert slice_["postCommands"] == []
    assert "command-unrooted" in [w["code"] for w in plan["warnings"]]


# ---------------------------------------------------------------------
# tan-cli#551 -- every -DALP_* define must reach the configure.
# ---------------------------------------------------------------------


def test_baremetal_configure_carries_the_alp_cache_defines(
        tmp_path: Path) -> None:
    """The `NAME=VALUE` defines ride the configure command line, where a
    cmake cache entry belongs. Not one of them was emitted before
    (tan-cli#551): the app configured with `ALP_SOM_SKU=[]`,
    `ALP_CORE_ID=[]` and still reported `status: ok, rc: 0`."""
    _, slice_ = _baremetal_plan(tmp_path)
    args = slice_["command"]["args"]

    assert "-DALP_SOM_SKU=E1M-AEN801" in args
    assert "-DALP_SOM_FAMILY=alif-ensemble" in args
    assert "-DALP_CORE_ID=m55_hp" in args
    assert "-DALP_TOOLCHAIN=arm-zephyr-eabi" in args


def test_baremetal_compile_guards_are_never_bare_cmake_d_args(
        tmp_path: Path) -> None:
    """`-DALP_BOARD_E1M_EVK` (no `=value`) is NOT a legal cmake command
    line argument -- cmake exits 1 with `Parse error in command line
    argument: ALP_BOARD_E1M_EVK / Should be: VAR:type=value`, which would
    turn tan-cli#551's silent drop into a hard configure failure. The
    guard must therefore arrive as a real compiler definition instead,
    and never as a bare `-D` on the command line."""
    _, slice_ = _baremetal_plan(tmp_path)

    for arg in slice_["command"]["args"]:
        if arg.startswith("-DALP_"):
            assert "=" in arg, (
                f"bare cmake -D argument {arg!r} -- cmake rejects a -D "
                f"with no =value")


def test_baremetal_compile_guards_reach_the_preprocessor(
        tmp_path: Path) -> None:
    """`ALP_BOARD_<SLUG>` gates `include/alp/board.h` with `#if
    defined(...)`, so it must be a COMPILER definition, not a cmake cache
    variable (which the preprocessor never sees). It arrives via a
    generated `alp-baremetal.cmake` the configure pulls in with
    `-DCMAKE_PROJECT_INCLUDE=` -- an artefact the build command actually
    READS, unlike the `cmake-args.txt` removed in #1278."""
    _, slice_ = _baremetal_plan(tmp_path)

    assert [a["path"] for a in slice_["configArtefacts"]] == \
        ["build/m55_hp-baremetal/alp-baremetal.cmake"]
    contents = slice_["configArtefacts"][0]["contents"]
    assert "add_compile_definitions(ALP_BOARD_E1M_EVK)" in contents

    # Absolute + tokened: CMake resolves a relative CMAKE_PROJECT_INCLUDE
    # against the SOURCE dir of the project() that pulls it in (the app's
    # tree), not against the slice's build dir.
    assert ("-DCMAKE_PROJECT_INCLUDE="
            "${PROJECT_ROOT}/build/m55_hp-baremetal/alp-baremetal.cmake") \
        in slice_["command"]["args"]


def test_baremetal_configure_never_sets_cmake_c_flags(
        tmp_path: Path) -> None:
    """The guards must NOT be delivered as `-DCMAKE_C_FLAGS=-DALP_...`:
    setting that variable on the command line seeds the cache entry
    itself, so a firmware toolchain file's `CMAKE_C_FLAGS_INIT`
    (`-mcpu=cortex-m55 -mfloat-abi=hard`, ...) would never be applied and
    the slice would silently build for the wrong core."""
    _, slice_ = _baremetal_plan(tmp_path)

    for arg in slice_["command"]["args"]:
        assert not arg.startswith("-DCMAKE_C_FLAGS")
        assert not arg.startswith("-DCMAKE_CXX_FLAGS")
        assert not arg.startswith("-DCMAKE_ASM_FLAGS")


def test_baremetal_cmake_hint_comments_never_reach_the_argv(
        tmp_path: Path) -> None:
    """`--emit cmake-args` is a TEXT surface: it opens with a
    `# Auto-generated ...` banner and `libraries.baremetal_cmake_args`
    adds `# library <name>: ...` prose. None of that is an argument --
    a `#`-prefixed token handed to cmake becomes a stray source-dir
    argument, not a define."""
    _, slice_ = _baremetal_plan(tmp_path)

    for arg in slice_["command"]["args"]:
        assert not arg.startswith("#"), arg


# ---------------------------------------------------------------------
# End-to-end: run what the plan says, then look at the build tree.
# ---------------------------------------------------------------------


@pytest.mark.skipif(shutil.which("cmake") is None,
                    reason="cmake not on PATH")
def test_baremetal_plan_actually_produces_a_binary(tmp_path: Path) -> None:
    """The whole point, executed rather than asserted about: materialise
    the plan's artefacts, run `command` then every `postCommands` step
    the way an executor does (cwd = the slice's buildDir, tokens
    substituted), and require a real executable under the directory
    `artifacts.outputDir` names.

    Uses the HOST compiler -- this pins the planner's command shape and
    the artifacts contract, not any cross toolchain.
    """
    (tmp_path / "src").mkdir()
    (tmp_path / "src" / "main.c").write_text(
        "int main(void) { return 0; }\n", encoding="utf-8")
    (tmp_path / "src" / "CMakeLists.txt").write_text(textwrap.dedent("""
        cmake_minimum_required(VERSION 3.20)
        project(demo C)
        message(STATUS "SEEN_ALP_CORE_ID=[${ALP_CORE_ID}]")
        message(STATUS "SEEN_ALP_SOM_SKU=[${ALP_SOM_SKU}]")
        get_directory_property(_defs COMPILE_DEFINITIONS)
        message(STATUS "SEEN_ALP_DEFS=[${_defs}]")
        add_executable(demo main.c)
    """), encoding="utf-8")
    path = _write_board(tmp_path, AEN801_BAREMETAL)
    project = load_board_yaml(path)
    plan = json.loads(emit_build_plan(
        project, board_yaml=path, build_root=Path("build")))
    slice_ = next(s for s in plan["slices"] if s["backend"] == "baremetal")

    def detoken(tok: str) -> str:
        return (tok.replace("${PROJECT_ROOT}", str(tmp_path))
                   .replace("${SDK_ROOT}", str(REPO)))

    # The consumer's materialise step: byte-write every artefact contents.
    for art in slice_["configArtefacts"]:
        dest = tmp_path / art["path"]
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(art["contents"], encoding="utf-8")

    stdout = ""
    for step in [slice_["command"], *slice_["postCommands"]]:
        cwd = tmp_path / step["cwd"]
        cwd.mkdir(parents=True, exist_ok=True)
        proc = subprocess.run(
            [step["tool"], *(detoken(a) for a in step["args"])],
            cwd=cwd, capture_output=True, text=True)
        assert proc.returncode == 0, (
            f"{step['tool']} {step['args']}\n"
            f"{proc.stdout[-4000:]}\n{proc.stderr[-4000:]}")
        stdout += proc.stdout

    # tan-cli#551: the defines really landed, cache entries AND guard.
    assert "SEEN_ALP_CORE_ID=[m55_hp]" in stdout
    assert "SEEN_ALP_SOM_SKU=[E1M-AEN801]" in stdout
    assert "SEEN_ALP_DEFS=[ALP_BOARD_E1M_EVK]" in stdout

    # tan-cli#550: a real binary, exactly where `artifacts` promised, and
    # the build tree is NOT nested a level down.
    build_dir = tmp_path / slice_["buildDir"]
    assert (build_dir / "CMakeCache.txt").is_file()
    assert not (build_dir / "build").exists()
    assert (tmp_path / slice_["artifacts"]["compileCommands"]).is_file()
    out_dir = tmp_path / slice_["artifacts"]["outputDir"]
    assert out_dir.is_dir()
    assert [p.name for p in out_dir.iterdir()] == ["demo"]
