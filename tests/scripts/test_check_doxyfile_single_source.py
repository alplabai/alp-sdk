# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_doxyfile_single_source.py."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_doxyfile_single_source as gate  # noqa: E402

_HEREDOC = (
    "PROJECT_NAME = \"Alp SDK\"\n"
    "OPTIMIZE_OUTPUT_FOR_C = YES\n"
    "DOT_GRAPH_MAX_NODES = 200\n"
    "JAVADOC_AUTOBRIEF = YES\n"
    "WARN_AS_ERROR = FAIL_ON_WARNINGS\n"
)


def _seed_doxyfile(root: Path) -> None:
    d = root / "docs" / "doxygen"
    d.mkdir(parents=True)
    (d / "Doxyfile").write_text("PROJECT_NAME = \"Alp SDK\"\n", encoding="utf-8")


def test_no_doxyfile_means_no_problems_even_with_a_heredoc(tmp_path):
    """The gate is only meaningful once the single source exists."""
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "test-all.sh").write_text(_HEREDOC, encoding="utf-8")
    assert gate.find_problems(tmp_path) == []


def test_reembedded_heredoc_in_scripts_sh_is_flagged(tmp_path):
    _seed_doxyfile(tmp_path)
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "test-all.sh").write_text(_HEREDOC, encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "test-all.sh" in problems[0]


def test_reembedded_heredoc_in_workflow_yml_is_flagged(tmp_path):
    _seed_doxyfile(tmp_path)
    wf = tmp_path / ".github" / "workflows"
    wf.mkdir(parents=True)
    (wf / "pr-doxygen.yml").write_text(_HEREDOC, encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "pr-doxygen.yml" in problems[0]


def test_single_incidental_mention_does_not_trip_the_gate(tmp_path):
    """One tag name mentioned in passing (e.g. a comment) is below the
    distinct-tag threshold -- it must not false-positive."""
    _seed_doxyfile(tmp_path)
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "unrelated.sh").write_text(
        "# see WARN_AS_ERROR in docs/doxygen/Doxyfile\n", encoding="utf-8"
    )
    assert gate.find_problems(tmp_path) == []


def test_referencing_the_single_source_passes(tmp_path):
    _seed_doxyfile(tmp_path)
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "test-all.sh").write_text(
        "cat docs/doxygen/Doxyfile | doxygen -\n", encoding="utf-8"
    )
    assert gate.find_problems(tmp_path) == []


def test_real_repo_passes():
    """Smoke test against the actual repo tree (post-#970 fix)."""
    assert gate.find_problems(REPO) == []


def test_cli_exits_nonzero_on_violation(tmp_path):
    _seed_doxyfile(tmp_path)
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "test-all.sh").write_text(_HEREDOC, encoding="utf-8")
    result = subprocess.run(
        [sys.executable, str(REPO / "scripts" / "check_doxyfile_single_source.py"),
         "--root", str(tmp_path)],
        capture_output=True, text=True,
    )
    assert result.returncode == 1
    assert "test-all.sh" in result.stderr
