# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_diagnostic_schema.py.

This gate is NOT run by any GitHub Actions workflow -- it only runs via
`scripts/test-all.sh`'s local registry loop -- so this file is the sole CI
coverage for it (it runs under `pr-metadata-validate.yml:462` and
`cross-platform-zephyr.yml:277`).

Locks in the `--fixture` guard the in-process rewrite (#1363) had to grow to
replace `click.Path(exists=True, dir_okay=False)`: a bad `--fixture` must
come back as a clean gate error (rc=1, one message on stdout), never a raw
traceback out of `Path.is_file()`/`machine_json_for_board_yaml()`.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_diagnostic_schema.py"


def _run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        cwd=REPO, capture_output=True, text=True, check=False,
    )


def test_fixture_pointing_at_a_directory_fails_cleanly() -> None:
    fixture = "tests/fixtures/board_yaml_bad"
    proc = _run("--fixture", fixture)
    assert proc.returncode == 1
    assert proc.stdout == (
        f"check_diagnostic_schema: --fixture {fixture} is not a file; "
        f"point it at a known-bad board.yaml under "
        f"tests/fixtures/board_yaml_bad/.\n"
    )
    assert proc.stderr == ""


def test_fixture_that_does_not_exist_fails_cleanly() -> None:
    fixture = "tests/fixtures/board_yaml_bad/NOPE.yaml"
    proc = _run("--fixture", fixture)
    assert proc.returncode == 1
    assert proc.stdout == (
        f"check_diagnostic_schema: --fixture {fixture} is not a file; "
        f"point it at a known-bad board.yaml under "
        f"tests/fixtures/board_yaml_bad/.\n"
    )
    assert proc.stderr == ""


def test_default_fixture_passes() -> None:
    """Non-vacuity smoke test against the real tree (subprocess, mirrors how
    scripts/test-all.sh invokes it)."""
    proc = _run()
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert proc.stdout.startswith("OK   ")
