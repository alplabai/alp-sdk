# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_diagnostic_narratives.py (issue #1207).

Locks in the non-vacuity property the gate exists for: a placeholder-only
docs/diagnostics/ALP-Bxxx.md page ("narrative to be added") must fail, and a
page with a real narrative + a Fix/Resolution/Remedy section must pass.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_diagnostic_narratives.py"

sys.path.insert(0, str(REPO / "scripts"))
import check_diagnostic_narratives as gate  # noqa: E402

_PLACEHOLDER = "# ALP-B001\n\n(Diagnostic landing page -- narrative to be added.)\n"

_REAL_PAGE = """\
# ALP-B001: required key is missing

The invariant that failed, in plain prose.

## Fix

What to do about it.
"""


def _write(root: Path, rel: str, body: str) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body, encoding="utf-8")
    return path


def test_clean_tree_passes(tmp_path: Path) -> None:
    _write(tmp_path, "docs/diagnostics/ALP-B001.md", _REAL_PAGE)
    assert gate.find_problems(tmp_path) == []


def test_placeholder_page_fails(tmp_path: Path) -> None:
    _write(tmp_path, "docs/diagnostics/ALP-B001.md", _PLACEHOLDER)
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "docs/diagnostics/ALP-B001.md" in problems[0]
    assert "narrative to be added" in problems[0]


def test_page_without_fix_section_fails(tmp_path: Path) -> None:
    """Dodging the literal placeholder string with no remediation at all
    must still fail -- a diagnosis with no fix is still not useful."""
    _write(
        tmp_path,
        "docs/diagnostics/ALP-B001.md",
        "# ALP-B001: required key is missing\n\nSome narrative, no fix section.\n",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "no '## Fix'" in problems[0]


def test_minimal_real_page_like_ALP_B007_passes(tmp_path: Path) -> None:
    """The shape docs/diagnostics/ALP-B007.md already ships: a bare `# CODE`
    heading, one intro paragraph, and a `## Fix` section -- must not be
    over-rejected for brevity."""
    _write(
        tmp_path,
        "docs/diagnostics/ALP-B007.md",
        "# ALP-B007\n\n"
        "The selected board preset does not list the selected SoM preset's "
        "`family:` in `hosts_som_families:`.\n\n"
        "## Fix\n\n"
        "Use a board preset that hosts the SoM family.\n",
    )
    assert gate.find_problems(tmp_path) == []


def test_real_repo_diagnostic_pages_pass() -> None:
    """Non-vacuity smoke test against the real tree (subprocess, mirrors how
    CI invokes it)."""
    proc = subprocess.run(
        [sys.executable, str(SCRIPT)], capture_output=True, text=True,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "OK" in proc.stdout
