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

from alp_orchestrate import (OrchestratorError, emit_build_plan,  # noqa: E402
                             load_board_yaml)

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
    The ONE path the configure line FORCES on every generator is
    reported; the ELF's NAME stays unreported because the app's own
    CMakeLists.txt picks it and this emitter must never invent one."""
    _, slice_ = _baremetal_plan(tmp_path)

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
    #
    # `$<1:...>`-wrapped: a multi-config generator (Visual Studio --
    # the DEFAULT on Windows -- Xcode, Ninja Multi-Config) appends a
    # per-config subdirectory to a plain value, so the plan would say
    # `<buildDir>/output` while the binary sat in `output/Debug/`.
    # CMake suppresses that append when a generator expression is used.
    assert ("-DCMAKE_RUNTIME_OUTPUT_DIRECTORY="
            "$<1:${PROJECT_ROOT}/build/m55_hp-baremetal/output>") in args


def test_baremetal_artifacts_do_not_promise_a_compile_database(
        tmp_path: Path) -> None:
    """`compileCommands` must stay null on baremetal even though the
    configure asks for it.

    CMake implements `CMAKE_EXPORT_COMPILE_COMMANDS` "only by Makefile
    Generators and Ninja Generators.  It is ignored on other
    generators", and this planner does not choose the generator -- so on
    Windows, whose default generator is Visual Studio, the file is never
    written. Naming the path anyway is the same artifacts-lie class
    tan-cli#550 was filed about, pointed the other way; it reddened
    `python-smoke (windows-latest)` on this branch's first push."""
    _, slice_ = _baremetal_plan(tmp_path)
    assert slice_["artifacts"]["compileCommands"] is None
    # The arg itself stays: harmless, and a real convenience on the
    # generators that DO honour it.
    assert "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON" in slice_["command"]["args"]


def test_baremetal_configure_does_not_warn_about_its_own_defines(
        tmp_path: Path) -> None:
    """Every `-D` on this configure is set by the PLANNER, not the
    customer.  An app that consumes none of them made CMake end each
    configure with `CMake Warning: Manually-specified variables were not
    used by the project: ALP_SOM_FAMILY, ALP_TOOLCHAIN, ...` -- noise
    about the SDK's own behaviour that the customer can neither act on
    nor silence."""
    _, slice_ = _baremetal_plan(tmp_path)
    assert "--no-warn-unused-cli" in slice_["command"]["args"]


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

    # ...and neither an artifact path nor a config artefact is promised
    # on top of it: nothing will ever configure that build dir, so every
    # one of them would be a dangling path pinned by a command that will
    # never run -- the same artifacts-lie class tan-cli#550 is about.
    assert all(v is None for v in slice_["artifacts"].values()), \
        slice_["artifacts"]
    assert slice_["configArtefacts"] == []


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
    out_dir = tmp_path / slice_["artifacts"]["outputDir"]
    assert out_dir.is_dir()
    # Directly in `output/`, NOT in an `output/<CONFIG>/` subdirectory --
    # the `$<1:...>` wrap is what holds this on a multi-config generator.
    #
    # Assert the SHAPE, not a filename.  The host toolchain picks the
    # suffix: `demo` under gcc/clang, `demo.exe` PLUS a `demo.pdb` under
    # MSVC, which is what Windows CI's default Visual Studio generator
    # emits.  What this pins is that the entries are FILES sitting
    # directly in `output/` with no per-config subdirectory beside them
    # -- exactly what the generator expression buys.  Hard-coding `demo`
    # was a POSIX assumption and reddened python-smoke (windows-latest).
    entries = sorted(out_dir.iterdir(), key=lambda p: p.name)
    assert entries, "outputDir is empty -- the executable did not land there"
    assert all(p.is_file() for p in entries), (
        f"a per-config subdirectory appeared inside outputDir: "
        f"{[p.name for p in entries]}")
    assert any(p.stem == "demo" for p in entries), (
        f"no `demo` executable in outputDir: {[p.name for p in entries]}")


def test_an_empty_output_dir_does_not_mean_the_slice_built_nothing(
        tmp_path: Path) -> None:
    """Executed disproof of the inverse reading of `artifacts.outputDir`.

    `CMAKE_RUNTIME_OUTPUT_DIRECTORY` governs `add_executable()` targets
    ONLY. A firmware app written the way real ones are -- a static core
    library plus a custom link step producing the flashable image --
    builds cleanly and never creates `output/` at all. The schema,
    CHANGELOG and `docs/architecture.md` all used to say an empty
    `outputDir` "means it produced no binary"; a consumer implementing
    that sentence would fail this build, which is the exact inverse of
    the tan-cli#550 defect."""
    (tmp_path / "src").mkdir()
    (tmp_path / "src" / "main.c").write_text(
        "int main(void) { return 0; }\n", encoding="utf-8")
    (tmp_path / "src" / "CMakeLists.txt").write_text(textwrap.dedent("""
        cmake_minimum_required(VERSION 3.20)
        project(fw C)
        add_library(fwcore STATIC main.c)
        # Stand-in for the objcopy/link step a real firmware app runs to
        # produce its flashable image: portable, no add_executable().
        add_custom_command(
          OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fw.elf
          COMMAND ${CMAKE_COMMAND} -E copy
                  $<TARGET_FILE:fwcore> ${CMAKE_CURRENT_BINARY_DIR}/fw.elf
          DEPENDS fwcore)
        add_custom_target(fw ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/fw.elf)
    """), encoding="utf-8")
    path = _write_board(tmp_path, AEN801_BAREMETAL)
    project = load_board_yaml(path)
    plan = json.loads(emit_build_plan(
        project, board_yaml=path, build_root=Path("build")))
    slice_ = next(s for s in plan["slices"] if s["backend"] == "baremetal")

    def detoken(tok: str) -> str:
        return (tok.replace("${PROJECT_ROOT}", str(tmp_path))
                   .replace("${SDK_ROOT}", str(REPO)))

    for art in slice_["configArtefacts"]:
        dest = tmp_path / art["path"]
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(art["contents"], encoding="utf-8")

    for step in [slice_["command"], *slice_["postCommands"]]:
        cwd = tmp_path / step["cwd"]
        cwd.mkdir(parents=True, exist_ok=True)
        proc = subprocess.run(
            [step["tool"], *(detoken(a) for a in step["args"])],
            cwd=cwd, capture_output=True, text=True)
        assert proc.returncode == 0, (
            f"{step['tool']} {step['args']}\n"
            f"{proc.stdout[-4000:]}\n{proc.stderr[-4000:]}")

    build_dir = tmp_path / slice_["buildDir"]
    # It really built: an object file, an archive and the image.  The
    # object suffix is the toolchain's, not POSIX's -- `.o` under
    # gcc/clang, `.obj` under MSVC (which is what Windows CI's default
    # Visual Studio generator emits; globbing only `*.o` reddened
    # python-smoke (windows-latest)).
    assert (build_dir / "fw.elf").is_file()
    objs = [*build_dir.rglob("*.o"), *build_dir.rglob("*.obj")]
    assert objs, "no object file was produced"
    assert list(build_dir.rglob("*fwcore*")), "no archive was produced"
    # ...and `output/` was never created.
    assert not (tmp_path / slice_["artifacts"]["outputDir"]).exists()


# ---------------------------------------------------------------------
# #1889 -- an app-less `os: baremetal` core must be refused at
# validate time, not silently skipped at build time.
#
# `_resolve_topology_for_core` (loader.py) merges a project's `cores.<id>`
# entry OVER the SoM preset's `topology.<id>` default; a project entry
# that overrides `os:` to `baremetal` without its own `app:` still
# inherits the topology default's `app:` -- `alp-stock-shim` on every
# Cortex-M slot, `alp-image-edge` on every Cortex-A slot (every
# metadata/e1m_modules/<SKU>.yaml agrees). Neither is a bare-metal app,
# and no bare-metal stock default exists, so `_enforce_loader_rules`'s
# `not slice_.app` guard alone never fires here -- the inherited token
# is truthy. Confirmed against `orchestrator._slice_command`: a
# baremetal slice with no `app:` of its own returns `command: None`
# and is carried as a silently-skipped slice, never a build failure.
# ---------------------------------------------------------------------


def test_baremetal_core_with_no_app_of_its_own_is_refused_at_load(
        tmp_path: Path) -> None:
    """The exact #1889 shape: `os: baremetal` with no `app:` on an
    M-core slot that has a zephyr stock-shim topology default."""
    path = _write_board(tmp_path, """
        som:
          sku: E1M-AEN801
          hw_rev: r1

        preset: e1m-evk

        cores:
          m55_he:
            os: baremetal
          m55_hp:
            os: zephyr
            app: .
    """)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)

    msg = str(excinfo.value)
    assert "m55_he" in msg
    assert "app:" in msg
    assert "alp-stock-shim" in msg


def test_baremetal_core_with_no_app_on_an_a_core_slot_is_refused(
        tmp_path: Path) -> None:
    """Same shape on the OTHER stock token: a Cortex-A slot's topology
    default is `alp-image-edge` (the yocto stock image), equally not a
    bare-metal app."""
    path = _write_board(tmp_path, """
        som:
          sku: E1M-AEN801
          hw_rev: r1

        preset: e1m-evk

        cores:
          a32_cluster:
            os: baremetal
          m55_hp:
            os: zephyr
            app: .
    """)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)

    msg = str(excinfo.value)
    assert "a32_cluster" in msg
    assert "alp-image-edge" in msg


def test_baremetal_core_with_a_real_app_of_its_own_still_loads(
        tmp_path: Path) -> None:
    """The fix must not reject a genuine bare-metal `app:` -- only the
    inherited stock tokens."""
    (tmp_path / "src").mkdir()
    (tmp_path / "src" / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\nproject(demo C)\n",
        encoding="utf-8")
    path = _write_board(tmp_path, """
        som:
          sku: E1M-AEN801
          hw_rev: r1

        preset: e1m-evk

        cores:
          m55_he:
            os: baremetal
            app: ./src
          m55_hp:
            os: zephyr
            app: .
    """)
    project = load_board_yaml(path)  # must not raise
    assert project.cores["m55_he"].app == "./src"
