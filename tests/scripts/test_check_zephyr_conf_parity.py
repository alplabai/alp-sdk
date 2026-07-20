# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_zephyr_conf_parity.py -- the CMakeLists.txt
<-> build-plan `alp.conf` byte-parity gate (docs/adr/0020-sdk-owns-build-
execution.md addendum: the CMakeLists.txt-driven path stays `--core`-scoped
for twister/bare-`west build` consumers, the planner's `EXTRA_CONF_FILE`
wiring serves `tan`-driven builds, and this pins the two can never diverge).
"""
import importlib.util
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


def test_default_corpus_byte_identical():
    proc = _run()
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "byte-identical" in proc.stdout


def test_finds_every_core_scoped_example():
    # A regression here (an example silently dropping out of the corpus,
    # e.g. a `--core` regex mismatch on a new formatting variant) is as
    # dangerous as a byte mismatch -- it would just stop checking silently.
    proc = _run()
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "0 example(s)" not in proc.stdout
    ok_count = proc.stdout.count("OK   examples/")
    assert ok_count >= 90, (
        f"expected ~92 --core-scoped examples, only found {ok_count} -- "
        f"the discovery regex may have regressed")


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
