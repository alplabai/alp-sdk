# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_diagnostic_emission_coverage.py.

The gate greps scripts/alp_cli/validator.py for `code="ALP-Bxxx"` emission
sites and asserts each has a docs/diagnostics/ALP-Bxxx.md page. Both cases
below run against a scaffolded tmp_path tree, not the real repo.

Run locally:

    python -m pytest tests/scripts/test_check_diagnostic_emission_coverage.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_diagnostic_emission_coverage as gate  # noqa: E402


def _scaffold(tmp_path: Path, codes: list[str], documented: list[str]) -> None:
    validator_dir = tmp_path / "scripts" / "alp_cli"
    validator_dir.mkdir(parents=True)
    body = "\n".join(f'    raise ValidationError(code="{c}", msg="x")' for c in codes)
    (validator_dir / "validator.py").write_text(body + "\n", encoding="utf-8")

    diag_dir = tmp_path / "docs" / "diagnostics"
    diag_dir.mkdir(parents=True)
    for c in documented:
        (diag_dir / f"{c}.md").write_text(f"# {c}\n\n## Fix\n\nDo the thing.\n", encoding="utf-8")


def test_clean_tree_passes(tmp_path: Path) -> None:
    _scaffold(tmp_path, codes=["ALP-B001", "ALP-B002"], documented=["ALP-B001", "ALP-B002"])
    assert gate.find_problems(tmp_path) == []


def test_emission_with_no_doc_page_fails(tmp_path: Path) -> None:
    _scaffold(tmp_path, codes=["ALP-B001", "ALP-B900"], documented=["ALP-B001"])
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "ALP-B900" in problems[0]
    assert "docs/diagnostics/ALP-B900.md does not exist" in problems[0]


def test_doc_with_no_emission_site_is_not_a_problem(tmp_path: Path) -> None:
    """A retired code can keep its landing page -- the gate is one-directional."""
    _scaffold(tmp_path, codes=["ALP-B001"], documented=["ALP-B001", "ALP-B999"])
    assert gate.find_problems(tmp_path) == []
