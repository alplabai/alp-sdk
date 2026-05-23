"""Unit tests for scripts/check_stub_issues.py."""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_stub_issues.py"


def _run(*args, **kw):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, **kw,
    )


def test_empty_tree_passes(tmp_path):
    """No backends at all -> exit 0."""
    (tmp_path / "src" / "backends").mkdir(parents=True)
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stderr


def test_stub_with_issue_passes(tmp_path):
    """Stub file naming the issue link -> exit 0."""
    d = tmp_path / "src" / "backends" / "adc"
    d.mkdir(parents=True)
    (d / "nxp_stub.c").write_text(
        "/*\n * @par Implementation status: NOT_IMPLEMENTED\n"
        " * @par Tracking: github.com/alplabai/alp-sdk/issues/42\n */\n"
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_stub_without_issue_fails(tmp_path):
    """*_stub.c without an issue ref -> exit 1."""
    d = tmp_path / "src" / "backends" / "adc"
    d.mkdir(parents=True)
    (d / "broken_stub.c").write_text("/* @par Implementation status: NOT_IMPLEMENTED */\n")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode != 0
    assert "broken_stub.c" in proc.stdout + proc.stderr
