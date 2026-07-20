# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_zephyr_conf_parity.py -- the CMakeLists.txt
<-> build-plan `alp.conf` byte-parity gate (docs/adr/0020-sdk-owns-build-
execution.md addendum: the CMakeLists.txt-driven path stays `--core`-scoped
for twister/bare-`west build` consumers, the planner's `EXTRA_CONF_FILE`
wiring serves `tan`-driven builds, and this pins the two can never diverge).
"""
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_zephyr_conf_parity.py"


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
