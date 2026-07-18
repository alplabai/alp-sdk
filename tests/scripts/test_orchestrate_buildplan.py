# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_orchestrate/ -- `--emit build-plan` (the
Wave C consumer contract: the `alp` CLI consumes this JSON instead of
re-implementing the planner), plus its app-path-anchoring (#596) and
Yocto app-only-target (#597) regression guards.

Split out of the orchestrator test suite as part of issue #460 / #673
Phase 3 (module-size reduction).

Run locally:

    python -m pytest tests/scripts/test_orchestrate_buildplan.py -v
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import REPO, V2N_HAPPY, _write_board  # noqa: E402

from alp_orchestrate import (                       # noqa: E402
    Orchestrator,
    load_board_yaml,
)


# ---------------------------------------------------------------------
# --emit build-plan (the Wave C consumer contract: the `alp` CLI
# consumes this JSON instead of re-implementing the planner).
# Settled 2026-06-04 with the alp-sdk-vscode team:
#   * camelCase keys, schemaVersion'd independently of board.yaml;
#   * GeneratedFile entries MUST carry `contents` (CLI materialise
#     stays pure IO);
#   * no `inputHash` (the CLI recomputes its cache key) and no
#     `sequential` (parallelism policy belongs to the CLI scheduler);
#   * one slice per non-`off` core; command-less slices carry
#     `command: null` + a warning instead of being dropped.
# ---------------------------------------------------------------------


V2N_BOOT_MCUBOOT = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33

boot:
  method: mcuboot
  signing:
    algorithm: rsa2048
    key_file: keys/dev_rsa.pem
"""


V2N_OFF_AND_COMMANDLESS = """
som:
  sku: E1M-V2N101

cores:
  a55_cluster:
    os: 'off'
  m33_sm:
    os: zephyr
"""


def test_emit_build_plan_happy(tmp_path: Path) -> None:
    """The plan carries the settled top-level shape, one slice per
    non-off core (sorted), the exact tool command per slice, and
    contents-bearing artefacts."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    out = emit_build_plan(project, board_yaml=path,
                          build_root=Path("build"))
    plan = _json.loads(out)

    assert plan["schemaVersion"] == 1
    assert plan["sku"] == "E1M-V2N101"
    assert plan["boardYaml"] == path.as_posix()
    assert plan["buildRoot"] == "build"
    assert isinstance(plan["warnings"], list)

    # Settled schema decisions: the CLI owns cache keys + parallelism.
    assert "sequential" not in plan
    for s in plan["slices"]:
        assert "inputHash" not in s

    # Slices sorted by coreId, one per non-off core.
    assert [s["coreId"] for s in plan["slices"]] == \
        ["a55_cluster", "m33_sm"]

    m33 = next(s for s in plan["slices"] if s["coreId"] == "m33_sm")
    assert m33["backend"] == "zephyr"
    assert m33["buildDir"] == "build/m33_sm-zephyr"
    assert m33["command"]["tool"] == "west"
    assert m33["command"]["args"][:2] == ["build", "-b"]
    assert m33["command"]["args"][-2:] == [
        "--", f"-DPython3_EXECUTABLE={sys.executable}",
    ]
    assert m33["command"]["cwd"] == m33["buildDir"]
    assert m33["env"]["ALP_SDK_ROOT"]
    confs = {a["path"]: a["contents"] for a in m33["configArtefacts"]}
    assert "build/m33_sm-zephyr/alp.conf" in confs
    assert "CONFIG_ALP_SDK=y" in confs["build/m33_sm-zephyr/alp.conf"]

    a55 = next(s for s in plan["slices"] if s["coreId"] == "a55_cluster")
    assert a55["backend"] == "yocto"
    assert a55["command"]["tool"] == "bitbake"
    assert a55["command"]["args"] == ["alp-image-edge"]
    confs = {a["path"]: a["contents"] for a in a55["configArtefacts"]}
    assert "build/a55_cluster-yocto/local.conf" in confs
    assert confs["build/a55_cluster-yocto/local.conf"].strip()

    # Shared artefacts carry contents (the CLI byte-writes them).
    shared = {a["path"]: a["contents"] for a in plan["sharedArtefacts"]}
    assert "build/generated/alp/system_ipc.h" in shared
    assert "build/generated/dts-reservations.dtsi" in shared
    assert "build/generated/dts-partitions.dtsi" in shared
    for contents in shared.values():
        assert contents.strip()


def test_emit_build_plan_carries_sdk_provenance(tmp_path: Path) -> None:
    """The envelope's `sdkVersion`/`sdkCommit` (additive, ADR 0014 -- no
    schemaVersion bump) trace a plan back to the planner that produced it:
    `sdkVersion` matches metadata/sdk_version.yaml's `version:` verbatim,
    and `sdkCommit` is either a short git commit or null (never a crash)
    when git is unavailable."""
    import json as _json
    import re
    from alp_orchestrate import emit_build_plan

    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    plan = _json.loads(emit_build_plan(
        project, board_yaml=path, build_root=Path("build")))

    sdk_version_yaml = (REPO / "metadata" / "sdk_version.yaml").read_text(
        encoding="utf-8")
    want_version = re.search(
        r"^version:\s*(\S+)", sdk_version_yaml, re.MULTILINE).group(1)
    assert plan["sdkVersion"] == want_version
    assert plan["sdkCommit"] is None or isinstance(plan["sdkCommit"], str)


def test_sdk_commit_degrades_to_none_when_git_unavailable(monkeypatch) -> None:
    """`_sdk_commit` never raises -- a missing `git` binary (or any
    subprocess failure) degrades to `None`, matching `build_receipt.
    _git_rev`'s robustness."""
    import subprocess
    from alp_orchestrate.buildplan import _sdk_commit

    def _raise(*a, **kw):
        raise FileNotFoundError("git")

    monkeypatch.setattr(subprocess, "run", _raise)
    assert _sdk_commit() is None


def test_sdk_version_degrades_to_none_when_metadata_missing(monkeypatch) -> None:
    """`_sdk_version` never raises -- an unreadable/absent
    `metadata/sdk_version.yaml` (e.g. packaged as a wheel with no adjacent
    metadata tree) degrades to `None`, matching `alp_cli._version`'s OSError
    guard, so provenance is best-effort and the emit never fails on it."""
    from pathlib import Path as _Path
    from alp_orchestrate.buildplan import _sdk_version

    def _raise(self, *a, **kw):
        raise FileNotFoundError("metadata/sdk_version.yaml")

    monkeypatch.setattr(_Path, "read_text", _raise)
    assert _sdk_version() is None


def test_emit_build_plan_stock_shim_resolves_to_sdk_app(tmp_path: Path) -> None:
    """A core left on the stock M-core shim (app: alp-stock-shim) gets a
    normal west command pointed at the SDK-owned shim app."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    board = """
name: stock-shim-board
som:
  sku: E1M-V2N101
  hw_rev: r1

cores:
  m33_sm:
    os: zephyr
    app: alp-stock-shim
"""
    path = _write_board(tmp_path, board)
    project = load_board_yaml(path)
    out = emit_build_plan(project, board_yaml=path, build_root=Path("build"))
    plan = _json.loads(out)

    m33 = next(s for s in plan["slices"] if s["coreId"] == "m33_sm")
    assert m33["command"]["tool"] == "west"
    assert m33["command"]["args"][:3] == [
        "build",
        "-b",
        "alp_e1m_v2n101_m33_sm/r9a09g056n48gbg/cm33",
    ]
    assert m33["command"]["args"][3] == str(
        REPO / "firmware" / "alp-stock-shim")
    assert m33["command"]["cwd"] == "build/m33_sm-zephyr"

    stock_warns = [w for w in plan["warnings"]
                   if w["code"] == "stock-shim-unimplemented"]
    assert stock_warns == []

    # Carried, not dropped: the slice still ships its alp.conf artefact.
    assert any(a["path"].endswith("alp.conf")
               for a in m33["configArtefacts"])


def test_emit_build_plan_deterministic(tmp_path: Path) -> None:
    """Spec parity with the other emits: byte-identical re-runs."""
    from alp_orchestrate import emit_build_plan

    path = _write_board(tmp_path, V2N_HAPPY)
    out_a = emit_build_plan(load_board_yaml(path), board_yaml=path,
                            build_root=Path("build"))
    out_b = emit_build_plan(load_board_yaml(path), board_yaml=path,
                            build_root=Path("build"))
    assert out_a == out_b


def test_emit_build_plan_writes_nothing(
    tmp_path: Path, monkeypatch
) -> None:
    """The emit is pure -- no build dirs, no config files, nothing."""
    from alp_orchestrate import emit_build_plan

    monkeypatch.chdir(tmp_path)
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    emit_build_plan(project, board_yaml=path, build_root=Path("build"))
    assert [p.name for p in tmp_path.iterdir()] == ["board.yaml"]


def test_emit_build_plan_matches_materialiser(
    tmp_path: Path, monkeypatch
) -> None:
    """By-construction parity: every artefact the plan carries is
    byte-identical to what the Orchestrator's materialise step writes
    to disk (the contract promised to the CLI side)."""
    import json as _json
    from alp_orchestrate import emit_build_plan
    import alp_orchestrate

    path = _write_board(tmp_path, V2N_HAPPY)
    build_root = tmp_path / "build"

    plan = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=build_root))

    # Materialise via the real fan_out (dispatch skipped: no tools).
    monkeypatch.setattr(alp_orchestrate.orchestrator.shutil, "which",
                        lambda name: None)
    orch = Orchestrator(load_board_yaml(path), build_root)
    orch.fan_out(parallel=False)

    artefacts = list(plan["sharedArtefacts"])
    for s in plan["slices"]:
        artefacts.extend(s["configArtefacts"])
    assert artefacts
    for entry in artefacts:
        on_disk = Path(entry["path"]).read_text(encoding="utf-8")
        assert on_disk == entry["contents"], \
            f"{entry['path']} diverges from the materialiser"


def test_emit_build_plan_off_core_excluded_commandless_warns(
    tmp_path: Path,
) -> None:
    """`off` cores never enter the plan; a slice the script cannot
    yet build is carried with `command: null` plus a machine-readable
    warning, so the CLI can still report the core."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    path = _write_board(tmp_path, V2N_OFF_AND_COMMANDLESS)
    project = load_board_yaml(path)
    # Simulate a SoM whose topology hasn't pinned this core's Zephyr
    # board target yet (the pending-HW-config TBD case): the loader
    # permits `board: None` for zephyr -- only `app:` is enforced --
    # and _slice_command then has nothing to hand `west build -b`.
    project.cores["m33_sm"].board = None
    # Use a real app so this isolates the board-missing -> no-command path;
    # the SoM preset would otherwise default this M-core to the stock shim,
    # which has its own warning code (see test_emit_build_plan_stock_shim_skipped).
    project.cores["m33_sm"].app = "./m33"
    plan = _json.loads(emit_build_plan(
        project, board_yaml=path, build_root=Path("build")))

    assert [s["coreId"] for s in plan["slices"]] == ["m33_sm"]
    m33 = plan["slices"][0]
    assert m33["command"] is None
    codes = [(w["code"], w.get("coreId")) for w in plan["warnings"]]
    assert ("no-command", "m33_sm") in codes


def test_emit_build_plan_carries_boot_sysbuild_conf(
    tmp_path: Path, monkeypatch
) -> None:
    """A `boot:` block surfaces as the build/alp_sysbuild.conf shared
    artefact -- and the materialiser writes the same file (this also
    pins the fix for emit_sysbuild_conf never being wired into
    _materialise_shared)."""
    import json as _json
    from alp_orchestrate import emit_build_plan
    import alp_orchestrate

    path = _write_board(tmp_path, V2N_BOOT_MCUBOOT)
    build_root = tmp_path / "build"
    plan = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=build_root))

    shared = {a["path"]: a["contents"] for a in plan["sharedArtefacts"]}
    sysbuild_path = (build_root / "alp_sysbuild.conf").as_posix()
    assert sysbuild_path in shared
    assert "SB_CONFIG_BOOTLOADER_MCUBOOT=y" in shared[sysbuild_path]

    monkeypatch.setattr(alp_orchestrate.orchestrator.shutil, "which",
                        lambda name: None)
    orch = Orchestrator(load_board_yaml(path), build_root)
    orch.fan_out(parallel=False)
    assert (build_root / "alp_sysbuild.conf").read_text(
        encoding="utf-8") == shared[sysbuild_path]


def test_zephyr_slice_command_wires_sysbuild_overlay(tmp_path: Path) -> None:
    """ADR 0014 Phase-3: a `boot:` block (-> build/alp_sysbuild.conf) makes
    the Zephyr slice command pass `--sysbuild` plus a `-DSB_CONF_FILE=`
    define naming that overlay; a project without one adds no sysbuild flag.

    SB_CONF_FILE must be ABSOLUTE and must sit after west's `--` separator:
    sysbuild resolves a relative SB_CONF_FILE against APP_DIR rather than the
    slice's cwd (issue #805).  Both shapes also pin CMake to the
    orchestrator's Python so a stale cache cannot select a west-less
    interpreter."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    # With boot: -> sysbuild overlay emitted -> command carries the flags.
    path = _write_board(tmp_path, V2N_BOOT_MCUBOOT)
    plan = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=Path("build")))
    z = next(s for s in plan["slices"]
             if s["backend"] == "zephyr" and s["command"])
    args = z["command"]["args"]
    assert args[:2] == ["build", "-b"]
    assert "--sysbuild" in args

    sb_conf = (Path("build") / "alp_sysbuild.conf").resolve()
    assert args[-3:] == [
        "--",
        f"-DPython3_EXECUTABLE={sys.executable}",
        f"-DSB_CONF_FILE={sb_conf}",
    ]
    # The overlay define is a CMake define, never a west flag: it has to
    # land AFTER `--`, and the path has to be absolute.
    assert args.index("--sysbuild") < args.index("--")
    assert sb_conf.is_absolute()

    # Without boot: -> no sysbuild overlay -> no flag, bare command.
    path2 = _write_board(tmp_path, V2N_HAPPY, name="board-noboot.yaml")
    plan2 = _json.loads(emit_build_plan(
        load_board_yaml(path2), board_yaml=path2, build_root=Path("build")))
    z2 = next(s for s in plan2["slices"]
              if s["backend"] == "zephyr" and s["command"])
    assert "--sysbuild" not in z2["command"]["args"]
    assert not any(a.startswith("-DSB_CONF_FILE=")
                   for a in z2["command"]["args"])
    assert z2["command"]["args"][-2:] == [
        "--", f"-DPython3_EXECUTABLE={sys.executable}",
    ]


# Every option the emitter is allowed to hand to `west build`: the exact set
# Zephyr 4.4's scripts/west_commands/build.py defines, plus the `--no-sysbuild`
# that argparse.BooleanOptionalAction derives from `--sysbuild`.
#
# `west build` is a ZEPHYR EXTENSION command -- the west package ships no
# build command at all -- so this set tracks the Zephyr revision in west.yml,
# NOT the west version (issue #805).
WEST_BUILD_FLAGS = {
    "-b", "--board", "-d", "--build-dir", "-t", "--target",
    "-p", "--pristine", "-c", "--cmake", "--cmake-only", "--cmake-opt",
    "-n", "--just-print", "--dry-run", "--recon",
    "-o", "--build-opt", "-S", "--snippet", "-s", "--source-dir",
    "-T", "--test-item", "--sysbuild", "--no-sysbuild", "--domain",
    "--shield", "--extra-conf", "--extra-dtc-overlay",
}


def test_zephyr_slice_command_invents_no_west_flags(tmp_path: Path) -> None:
    """Every `--flag` the plan emits before west's `--` separator must be one
    Zephyr's `west build` actually defines.

    The suite only ever inspects the emitted plan STRING -- nothing here runs
    `west build` -- so an invented flag used to sail through the gate and fail
    only on a real configure, which is exactly how `--sysbuild-config` shipped
    (issue #805): west forwarded the unknown argument to CMake, which died
    with `Unknown argument --sysbuild-config`.  Anything past `--` is a CMake
    define and is deliberately not checked here."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    for src, name in ((V2N_BOOT_MCUBOOT, "board-boot.yaml"),
                      (V2N_HAPPY, "board-noboot.yaml")):
        path = _write_board(tmp_path, src, name=name)
        plan = _json.loads(emit_build_plan(
            load_board_yaml(path), board_yaml=path,
            build_root=Path("build")))
        for slice_ in plan["slices"]:
            if slice_["backend"] != "zephyr" or not slice_["command"]:
                continue
            args = slice_["command"]["args"]
            west_args = args[:args.index("--")] if "--" in args else args
            bogus = [a for a in west_args
                     if a.startswith("-") and a not in WEST_BUILD_FLAGS]
            assert not bogus, (
                f"{name}: emitted west build option(s) Zephyr does not "
                f"define: {bogus}")


def test_cli_emit_build_plan(tmp_path: Path, capsys, monkeypatch) -> None:
    """`--emit build-plan` prints the JSON to stdout, rc 0, writes
    nothing to disk."""
    import json as _json
    from alp_orchestrate import main as orchestrate_main

    monkeypatch.chdir(tmp_path)
    path = _write_board(tmp_path, V2N_HAPPY)
    rc = orchestrate_main(["--input", str(path), "--emit", "build-plan"])
    out = capsys.readouterr().out
    assert rc == 0
    plan = _json.loads(out)
    assert plan["schemaVersion"] == 1
    assert [p.name for p in tmp_path.iterdir()] == ["board.yaml"]


# ---------------------------------------------------------------------
# Issue #596 -- app paths anchor on board.yaml, never the process CWD
# ---------------------------------------------------------------------


def test_emit_build_plan_app_paths_independent_of_cwd(
    tmp_path: Path, monkeypatch
) -> None:
    """The plan's slice `command` + `appDir` resolve relative to
    board.yaml's own directory, byte-identical no matter which
    directory the emitting process happens to be running from --
    the #596 repro (`west build`'s target used to fall back to the
    repo root because the CWD-anchored resolve missed the app dir and
    the parent CMakeLists.txt fallback silently matched the root)."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    project_dir = tmp_path / "project"
    project_dir.mkdir()
    (project_dir / "m33").mkdir()
    (project_dir / "m33" / "CMakeLists.txt").write_text("", encoding="utf-8")
    path = _write_board(project_dir, V2N_HAPPY)

    elsewhere = tmp_path / "somewhere-else-entirely"
    elsewhere.mkdir()

    # Same board.yaml, same build_root; only the CWD differs.
    monkeypatch.chdir(project_dir)
    plan_same_dir = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=Path("build")))

    monkeypatch.chdir(elsewhere)
    plan_other_dir = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=Path("build")))

    assert plan_same_dir == plan_other_dir

    m33 = next(s for s in plan_other_dir["slices"] if s["coreId"] == "m33_sm")
    # Correctly anchored on the project dir -- NOT the unrelated CWD, and
    # NOT the repo root (the historical parent-CMakeLists.txt fallback trap).
    args = m33["command"]["args"]
    assert args[args.index("--") - 1] == str(project_dir / "m33")
    assert m33["appDir"] == (project_dir / "m33").as_posix()


def test_slice_command_helpers_take_explicit_base_dir(tmp_path: Path) -> None:
    """`_resolve_app_path` / `_zephyr_app_dir` / `_slice_command` require
    an explicit `base_dir` and never fall back to `Path.cwd()` -- the
    exact seam issue #596 flagged (`orchestrator.py` used to call
    `Path.cwd()` directly)."""
    from alp_orchestrate.orchestrator import _resolve_app_path

    project_dir = tmp_path / "myproj"
    (project_dir / "src").mkdir(parents=True)

    resolved = _resolve_app_path("./src", project_dir)
    assert resolved == (project_dir / "src").resolve()

    # Absolute paths pass through untouched regardless of base_dir.
    abs_dir = tmp_path / "abs-app"
    abs_dir.mkdir()
    assert _resolve_app_path(str(abs_dir), project_dir) == abs_dir


def test_orchestrator_dispatch_anchors_on_board_yaml_not_cwd(
    tmp_path: Path, monkeypatch
) -> None:
    """The real (non-emit) dispatch path -- `Orchestrator(..., board_yaml=)`
    -- resolves the same way `emit_build_plan` does, so `west
    alp-build`/`west alp-build --emit build-plan` never disagree on the
    app path depending on which directory the shell happens to be in."""
    import alp_orchestrate

    project_dir = tmp_path / "project"
    project_dir.mkdir()
    (project_dir / "m33").mkdir()
    path = _write_board(project_dir, V2N_HAPPY)

    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    monkeypatch.chdir(elsewhere)

    monkeypatch.setattr(alp_orchestrate.orchestrator.shutil, "which",
                        lambda name: None)
    project = load_board_yaml(path)
    build_root = tmp_path / "build"
    orch = Orchestrator(project, build_root, board_yaml=path)
    assert orch.base_dir == project_dir.resolve()


# ---------------------------------------------------------------------
# Issue #597 -- Yocto app-only slices need a valid bitbake target
# ---------------------------------------------------------------------


YOCTO_APP_ONLY_NO_RECIPE = """
som:
  sku: E1M-V2N101
  hw_rev: r1

cores:
  a55_cluster:
    app: ./src
  m33_sm:
    os: "off"
"""


YOCTO_APP_ONLY_WITH_RECIPE = """
som:
  sku: E1M-V2N101
  hw_rev: r1

cores:
  a55_cluster:
    app: ./src
    recipe: alp-my-app
  m33_sm:
    os: "off"
"""


def test_yocto_app_only_without_recipe_blocks_not_bitbake_path(
    tmp_path: Path,
) -> None:
    """An app-only Yocto slice (`app:` set, no `image:`, no `recipe:`)
    must never emit `bitbake <path>` -- `app:` is a source directory, not
    a recipe name.  The plan blocks it explicitly instead (#597)."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    path = _write_board(tmp_path, YOCTO_APP_ONLY_NO_RECIPE)
    plan = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=Path("build")))

    a55 = next(s for s in plan["slices"] if s["coreId"] == "a55_cluster")
    assert a55["command"] is None
    assert a55["appDir"] == str((tmp_path / "src").resolve()).replace(
        "\\", "/")
    codes = [(w["code"], w.get("coreId")) for w in plan["warnings"]]
    assert ("yocto-recipe-missing", "a55_cluster") in codes
    # Never the historical bug shape.
    for s in plan["slices"]:
        if s["command"]:
            assert s["command"]["args"] != ["./src"]


def test_yocto_app_only_with_recipe_emits_valid_bitbake_target(
    tmp_path: Path,
) -> None:
    """An app-only Yocto slice that names its `recipe:` gets a real
    bitbake target; the app source dir is retained separately as
    `appDir` for tooling that wants it (#597)."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    path = _write_board(tmp_path, YOCTO_APP_ONLY_WITH_RECIPE)
    plan = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=Path("build")))

    a55 = next(s for s in plan["slices"] if s["coreId"] == "a55_cluster")
    assert a55["command"] == {
        "tool": "bitbake",
        "args": ["alp-my-app"],
        "cwd":  "build/a55_cluster-yocto",
    }
    assert a55["appDir"] == (tmp_path / "src").resolve().as_posix()
    assert not [w for w in plan["warnings"]
                if w["coreId"] == "a55_cluster"]


def test_yocto_image_and_app_both_set_image_wins_app_dir_retained(
    tmp_path: Path,
) -> None:
    """When both `image:` and `app:` are set, the recipe-valued `image:`
    is the actual bitbake target (never the app path); the app source
    dir is still carried as `appDir` for tooling, matching the
    acceptance criterion that image+app slices stay covered (#597)."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    path = _write_board(tmp_path, V2N_HAPPY)
    plan = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=Path("build")))

    a55 = next(s for s in plan["slices"] if s["coreId"] == "a55_cluster")
    assert a55["command"]["args"] == ["alp-image-edge"]
    assert a55["appDir"] == (tmp_path / "linux").resolve().as_posix()


def test_yocto_stock_image_app_token_still_resolves_without_recipe(
    tmp_path: Path,
) -> None:
    """The SoM preset's stock A-core default (`app: alp-image-edge`,
    no `image:`) is already a real bitbake recipe name, not a project
    source path -- it must keep building without requiring a `recipe:`
    override (regression guard: the #597 fix must not block the
    default/uncustomised topology every V2N/AEN A-core inherits)."""
    import json as _json
    from alp_orchestrate import emit_build_plan

    board = """
    som:
      sku: E1M-V2N101
      hw_rev: r1

    cores:
      m33_sm:
        os: "off"
    """
    path = _write_board(tmp_path, board)
    plan = _json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=Path("build")))

    a55 = next(s for s in plan["slices"] if s["coreId"] == "a55_cluster")
    assert a55["command"] == {
        "tool": "bitbake",
        "args": ["alp-image-edge"],
        "cwd":  "build/a55_cluster-yocto",
    }
    # Stock image token isn't a source path -- nothing to report.
    assert a55["appDir"] is None
