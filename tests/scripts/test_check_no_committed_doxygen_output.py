# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_no_committed_doxygen_output.py.

The gate parses `OUTPUT_DIRECTORY` out of a Doxyfile and asserts `git
ls-files` finds nothing tracked under it. Every case here builds a throwaway
git repo under tmp_path (the gate shells out to `git ls-files` against
`root`, so a real repo is required) rather than touching this checkout, and
proves the detector both fires on a real violation and can be pointed at a
directory it knows is tracked (docs/) to rule out a vacuous parse.

Run locally:

    python -m pytest tests/scripts/test_check_no_committed_doxygen_output.py -q
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_no_committed_doxygen_output as gate  # noqa: E402


def _git(root: Path, *args: str) -> None:
    subprocess.run(["git", *args], cwd=root, check=True, capture_output=True, text=True)


def _init_repo(root: Path) -> None:
    _git(root, "init", "-q")
    _git(root, "config", "user.email", "test@example.com")
    _git(root, "config", "user.name", "test")


def _write_doxyfile(root: Path, output_dir: str) -> None:
    doxydir = root / "docs" / "doxygen"
    doxydir.mkdir(parents=True, exist_ok=True)
    (doxydir / "Doxyfile").write_text(
        f"INPUT                  = include/alp\nOUTPUT_DIRECTORY       = {output_dir}\n",
        encoding="utf-8",
    )


def _commit_all(root: Path, message: str) -> None:
    _git(root, "add", "-A")
    _git(root, "commit", "-q", "-m", message)


def test_clean_tree_passes(tmp_path: Path) -> None:
    """No tracked file under OUTPUT_DIRECTORY -> no problems."""
    _init_repo(tmp_path)
    _write_doxyfile(tmp_path, "doxygen-out")
    (tmp_path / "README.md").write_text("hello\n", encoding="utf-8")
    _commit_all(tmp_path, "init")

    assert gate.find_problems(tmp_path) == []


def test_seeded_violation_fails(tmp_path: Path) -> None:
    """A tracked file under OUTPUT_DIRECTORY is exactly the drift #1573 fixed."""
    _init_repo(tmp_path)
    _write_doxyfile(tmp_path, "doxygen-out")
    outdir = tmp_path / "doxygen-out" / "html"
    outdir.mkdir(parents=True)
    (outdir / "index.html").write_text("<html></html>\n", encoding="utf-8")
    _commit_all(tmp_path, "accidentally commit generated html")

    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "doxygen-out" in problems[0]
    assert "doxygen-out/html/index.html" in problems[0]


def test_detector_catches_the_seeded_gap_pointed_at_a_known_tracked_dir(tmp_path: Path) -> None:
    """Guard the guard: point OUTPUT_DIRECTORY at `docs`, which the fixture
    itself tracks, and prove the detector actually reports it -- a broken
    `git ls-files` invocation or a parser that silently no-ops would instead
    make the clean-tree test above pass for the wrong reason."""
    _init_repo(tmp_path)
    _write_doxyfile(tmp_path, "docs")
    (tmp_path / "docs" / "doxygen" / "notes.md").write_text("notes\n", encoding="utf-8")
    _commit_all(tmp_path, "seed a tracked dir doxygen would never write to")

    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert problems[0].startswith("2 tracked file(s) under docs/")


def test_output_directory_follows_a_renamed_doxyfile_value(tmp_path: Path) -> None:
    """The parser reads OUTPUT_DIRECTORY rather than hardcoding 'doxygen-out',
    so a rename of the output dir is still caught (the drift class #1573 is
    guarding against)."""
    _init_repo(tmp_path)
    _write_doxyfile(tmp_path, "generated-docs")
    outdir = tmp_path / "generated-docs"
    outdir.mkdir()
    (outdir / "index.html").write_text("<html></html>\n", encoding="utf-8")
    _commit_all(tmp_path, "renamed output dir, still committed by mistake")

    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "generated-docs" in problems[0]
