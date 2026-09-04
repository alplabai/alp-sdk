# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_changelog_fragments.py (#1395).

This gate is a thin wrapper around assemble_changelog.load_fragments() --
its whole job is to fire at PR time on exactly what the release-time
assembler would refuse, instead of the drift surfacing at the release cut.
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

sys.path.insert(0, str(REPO / "scripts"))
import check_changelog_fragments as gate  # noqa: E402


def _repo(tmp_path: Path, fragments: dict[str, str]) -> Path:
    (tmp_path / "CHANGELOG.md").write_text("# Changelog\n", encoding="utf-8")
    frag = tmp_path / "changelog.d"
    frag.mkdir()
    (frag / "README.md").write_text("contract\n", encoding="utf-8")
    for name, body in fragments.items():
        (frag / name).write_text(body, encoding="utf-8")
    return tmp_path


def test_a_clean_tree_of_fragments_passes(tmp_path: Path) -> None:
    root = _repo(tmp_path, {
        "101.md": "### Added — Entry one\n\nBody.",
        "102.md": "### Fixed — Entry two\n\nBody.",
    })
    assert gate.find_problems(root) == []


def test_no_fragments_at_all_passes(tmp_path: Path) -> None:
    root = _repo(tmp_path, {})
    assert gate.find_problems(root) == []


def test_a_fragment_missing_its_heading_is_flagged(tmp_path: Path) -> None:
    root = _repo(tmp_path, {"201.md": "Just prose, no heading."})
    problems = gate.find_problems(root)
    assert len(problems) == 1
    assert "201.md" in problems[0]
    assert "heading" in problems[0]


def test_an_empty_fragment_is_flagged(tmp_path: Path) -> None:
    root = _repo(tmp_path, {"301.md": "   \n"})
    problems = gate.find_problems(root)
    assert len(problems) == 1
    assert "301.md" in problems[0]
    assert "empty" in problems[0]


def test_a_non_conforming_filename_is_flagged(tmp_path: Path) -> None:
    root = _repo(tmp_path, {"401.fixed.md": "### Fixed — Entry\n\nBody."})
    problems = gate.find_problems(root)
    assert len(problems) == 1
    assert "401.fixed.md" in problems[0]


def test_a_suffixed_filename_disambiguating_a_second_fragment_passes(tmp_path: Path) -> None:
    """alp-sdk#1941: a second fragment for one issue may add a `-slug` suffix
    instead of colliding with `<issue>.md` or being misnamed after the PR."""
    root = _repo(tmp_path, {
        "1909.md": "### Fixed — First fragment for #1909\n\nBody.",
        "1909-diagnostic-format-uri.md": "### Fixed — Second fragment for #1909\n\nBody.",
    })
    assert gate.find_problems(root) == []


def test_a_missing_category_enum_is_not_flagged(tmp_path: Path) -> None:
    """No category enum by design (#1395) -- any heading text is accepted."""
    root = _repo(tmp_path, {"501.md": "### Whatever — Entry\n\nBody."})
    assert gate.find_problems(root) == []


def test_missing_changelog_d_directory_is_flagged(tmp_path: Path) -> None:
    (tmp_path / "CHANGELOG.md").write_text("# Changelog\n", encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "changelog.d" in problems[0]


def test_main_exits_nonzero_on_the_real_gate_when_problems_exist(tmp_path: Path, monkeypatch, capsys) -> None:
    root = _repo(tmp_path, {"601.md": "no heading here"})
    monkeypatch.setattr(sys, "argv", ["check_changelog_fragments.py", "--root", str(root)])
    rc = gate.main()
    assert rc == 1
    assert "601.md" in capsys.readouterr().err
