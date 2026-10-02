# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_zephyr_conf_parity.py -- the pre-generated
`generated/alp.conf` <-> `alp_project.py --emit zephyr-conf` byte-parity gate
(docs/adr/0020-sdk-owns-build-execution.md addendum; #866 retired the
CMakeLists.txt configure-time bridge, so this also pins that it stays gone).
"""
import importlib.util
import os
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_zephyr_conf_parity.py"


def _load_gate():
    spec = importlib.util.spec_from_file_location("_czcp", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _run(*args):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, encoding="utf-8",
        env={**os.environ, "PYTHONIOENCODING": "utf-8"})


# issue alp-sdk#2328: test_default_corpus_byte_identical and
# test_finds_every_core_scoped_example both invoke the same no-arg `_run()`
# (the full gate over the whole example corpus, ~76s); share one run per
# module instead of paying for it twice. Any test needing a different
# invocation or a mutated corpus (e.g. test_flags_unscoped_emit) must not
# use this fixture.
@pytest.fixture(scope="module")
def default_corpus_run():
    return _run()


# The same live-repo run test-all.sh's required-gate-scripts stage executes
# (check_zephyr_conf_parity.py, no args), hence gate_duplicate.
@pytest.mark.gate_duplicate
def test_default_corpus_byte_identical(default_corpus_run):
    proc = default_corpus_run
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "byte-identical" in proc.stdout


def test_finds_every_example_core():
    # A regression here (an example silently dropping out of the corpus) is
    # as dangerous as a byte mismatch -- it would just stop checking silently.
    # Asked of the discovery function directly, not of a full gate run: the
    # count is the only thing this test adds, and it must not force the
    # corpus run that test_default_corpus_byte_identical owns.
    found = len(_load_gate().find_cases())
    assert found >= 90, (
        f"expected ~94 Zephyr example cores, only found {found} -- "
        f"the discovery may have regressed")


def test_flags_reintroduced_bridge(tmp_path):
    # A re-introduced configure-time `--emit zephyr-conf` in an example
    # CMakeLists.txt (scoped or not) must be caught.
    gate = _load_gate()
    bridged = tmp_path / "examples" / "bridged" / "CMakeLists.txt"
    bridged.parent.mkdir(parents=True)
    bridged.write_text(
        "execute_process(COMMAND python3 alp_project.py --input board.yaml "
        "--emit zephyr-conf --core m55_hp)\n", encoding="utf-8")
    clean = tmp_path / "examples" / "clean" / "CMakeLists.txt"
    clean.parent.mkdir(parents=True)
    clean.write_text("project(demo LANGUAGES C)\n", encoding="utf-8")

    bridges = gate.find_bridges(tmp_path)
    assert bridged in bridges, "re-added bridge was not flagged"
    assert clean not in bridges
