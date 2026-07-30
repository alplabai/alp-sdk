# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_zephyr_conf_parity.py -- the `proj-*.zephyr-conf.
snap` <-> `*.build-plan.snap` `alp.conf` byte-parity gate (docs/adr/0020-sdk-
owns-build-execution.md addendum: the CMakeLists.txt-driven path stays
`--core`-scoped for twister/bare-`west build` consumers, the planner's
`EXTRA_CONF_FILE` wiring serves `tan`-driven builds, and the two committed
emit-snapshot families that stand in for them can never diverge).
"""
import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_zephyr_conf_parity.py"


def _load_gate():
    spec = importlib.util.spec_from_file_location("_czcp", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _cmakelists(path: Path, body: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "execute_process(COMMAND python3 ${ALP_PROJECT} --input "
        "${CMAKE_CURRENT_SOURCE_DIR}/board.yaml " + body + ")\n",
        encoding="utf-8")
    return path


def _run(*args):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True)


def _zephyr_conf_snap(dir_: Path, bid: str, core_id: str, content: str) -> Path:
    """A synthetic `proj-<bid>.zephyr-conf.snap` with a single core section.
    `bid` must be one of `_PROJ_BOARD_YAML`'s known ids (aen/v2n/nsim) so the
    gate can resolve it to a board.yaml."""
    dir_.mkdir(parents=True, exist_ok=True)
    path = dir_ / f"proj-{bid}.zephyr-conf.snap"
    path.write_text(f"# --- core: {core_id} (zephyr) ---\n{content}",
                     encoding="utf-8")
    return path


def _build_plan_snap(dir_: Path, name: str, board_yaml: str, core_id: str,
                      content: str) -> Path:
    """A synthetic `<name>.build-plan.snap` with one Zephyr slice carrying a
    single `alp.conf` configArtefact."""
    dir_.mkdir(parents=True, exist_ok=True)
    path = dir_ / f"{name}.build-plan.snap"
    doc = {
        "boardYaml": board_yaml,
        "slices": [{
            "coreId": core_id,
            "configArtefacts": [
                {"path": f"build/{core_id}-zephyr/alp.conf", "contents": content},
            ],
        }],
    }
    path.write_text(json.dumps(doc), encoding="utf-8")
    return path


def test_default_corpus_compares_at_least_one_real_pair():
    # Replaces `test_default_corpus_currently_has_zero_overlap`, which pinned
    # the opposite state: check_emit_snapshots.py's PROJ (zephyr-conf) and ORCH
    # (build-plan) board sets shared no board.yaml, so this gate had nothing to
    # compare and failed rather than passing on an empty set. `rpmsg-v2n` now
    # carries BOTH goldens, so the overlap is real and the gate is green for a
    # reason.
    #
    # Asserted as ">= 1", not "== 1": a second overlapping board is exactly the
    # improvement this corpus wants, and a test forbidding it would make the
    # gate harder to strengthen than to leave weak. Zero is the state that must
    # never come back, and `test_zero_pairs_fails_loudly` below pins that.
    proc = _run()
    assert proc.returncode == 0, proc.stdout + proc.stderr
    match = re.search(r"(\d+) board/core pair\(s\)", proc.stdout + proc.stderr)
    assert match, proc.stdout + proc.stderr
    assert int(match.group(1)) >= 1, proc.stdout + proc.stderr


def test_zero_pairs_fails_loudly(tmp_path):
    # An empty snapshot dir must fail, not silently report success -- the
    # exact failure class this gate exists to prevent (issue: a gate that
    # iterates nothing and still prints OK).
    proc = _run("--snapshot-dir", str(tmp_path))
    assert proc.returncode != 0
    assert "0 board/core pairs" in proc.stdout + proc.stderr


def test_matching_pair_passes(tmp_path):
    content = "CONFIG_ALP_SDK=y\nCONFIG_LOG=y\n\n"
    _zephyr_conf_snap(tmp_path, "aen", "m55_he", content)
    _build_plan_snap(tmp_path, "synthetic",
                      "examples/aen/aen-analog-validate/board.yaml",
                      "m55_he", content)
    proc = _run("--snapshot-dir", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "byte-identical" in proc.stdout
    assert "examples/aen/aen-analog-validate/board.yaml" in proc.stdout


def test_diverging_pair_fails(tmp_path):
    _zephyr_conf_snap(tmp_path, "aen", "m55_he", "CONFIG_ALP_SDK=y\n\n")
    _build_plan_snap(tmp_path, "synthetic",
                      "examples/aen/aen-analog-validate/board.yaml",
                      "m55_he", "CONFIG_ALP_SDK=n\n\n")
    proc = _run("--snapshot-dir", str(tmp_path))
    assert proc.returncode != 0
    out = proc.stdout + proc.stderr
    assert "diverged" in out
    assert "examples/aen/aen-analog-validate/board.yaml" in out
    assert "m55_he" in out


def test_flags_unscoped_emit(tmp_path):
    # A re-introduced `--emit zephyr-conf` WITHOUT `--core` (the cross-core
    # Kconfig leak ADR-0020 retired) must be caught, not silently skipped by
    # the `--core`-scoped discovery. `_find_unscoped_emits` is that guard.
    gate = _load_gate()
    leaky = _cmakelists(
        tmp_path / "examples" / "leaky-demo" / "CMakeLists.txt",
        "--emit zephyr-conf")
    scoped = _cmakelists(
        tmp_path / "examples" / "scoped-demo" / "CMakeLists.txt",
        "--emit zephyr-conf --core m55_hp")

    leaks = gate._find_unscoped_emits(tmp_path)
    assert leaky in leaks, "unscoped `--emit zephyr-conf` was not flagged"
    assert scoped not in leaks, "a `--core`-scoped emit was wrongly flagged"
