# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_changelog_fragment_issue.py (#1957).

This gate checks one thing: a changelog.d/ fragment's filename leading
digits agree with the single `#N` its own heading cites, where that's
decidable at all.
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

sys.path.insert(0, str(REPO / "scripts"))
import check_changelog_fragment_issue as gate  # noqa: E402


def _repo(tmp_path: Path, fragments: dict[str, str]) -> Path:
    frag = tmp_path / "changelog.d"
    frag.mkdir()
    (frag / "README.md").write_text("contract\n", encoding="utf-8")
    for name, body in fragments.items():
        (frag / name).write_text(body, encoding="utf-8")
    return tmp_path


def test_a_matching_fragment_passes(tmp_path: Path) -> None:
    root = _repo(tmp_path, {
        "101.md": "### Added — Entry one (#101)\n\nBody.",
    })
    assert gate.find_problems(root) == []


def test_a_mismatched_fragment_is_flagged(tmp_path: Path) -> None:
    """alp-sdk#1957's own motivating case: changelog.d/1909.md's heading
    cited (#1700), not #1909."""
    root = _repo(tmp_path, {
        "1909.md": "### Added — a diagnostic warning (#1700)\n\nBody.",
    })
    problems = gate.find_problems(root)
    assert len(problems) == 1
    assert "1909.md" in problems[0]
    assert "#1909" in problems[0]
    assert "(#1700)" in problems[0]


def test_a_heading_with_no_citation_at_all_is_not_flagged(tmp_path: Path) -> None:
    """changelog.d/853.md's shape: a bare `### Changed` heading, no title,
    no `(#N)` -- nothing to compare against, so under-flag."""
    root = _repo(tmp_path, {
        "853.md": "### Changed\n\n- Documented something.",
    })
    assert gate.find_problems(root) == []


def test_a_non_heading_first_line_is_not_flagged(tmp_path: Path) -> None:
    """A fragment whose first non-blank line is not a `### ` heading at all
    (a structural defect `check_changelog_fragments.py` already owns) is
    skipped by this gate, even though its filename's leading digits would
    mismatch a `#N` mentioned in that first line."""
    root = _repo(tmp_path, {
        "500.md": "Not a heading, just prose mentioning #501.\n\nBody.",
    })
    assert gate.find_problems(root) == []


def test_a_non_digit_leading_filename_is_not_flagged(tmp_path: Path) -> None:
    """A fragment whose filename does not start with digits (a structural
    defect `check_changelog_fragments.py` already owns) is skipped by this
    gate, even though it has a normal single-citation heading."""
    root = _repo(tmp_path, {
        "notes.md": "### Fixed — Entry (#42)\n\nBody.",
    })
    assert gate.find_problems(root) == []


def test_a_heading_citing_a_range_is_not_flagged(tmp_path: Path) -> None:
    """changelog.d/1761.md's shape: `(#1757-#1783)` cites two distinct
    numbers -- ambiguous which one, if any, should be the filename; under-flag."""
    root = _repo(tmp_path, {
        "1761.md": "### Fixed — a sweep (#1757-#1783)\n\nBody.",
    })
    assert gate.find_problems(root) == []


def test_a_multi_citation_heading_outside_the_cited_range_is_flagged(tmp_path: Path) -> None:
    """changelog.d/1940.md's real shape: a heading citing two distinct
    issues, `(#1848, #1814)`, where the filename's leading digits (1940)
    are neither one of those two numbers nor inside the 1814..1848 span
    between them -- unlike changelog.d/1761.md's genuine range citation,
    there is no ambiguity to under-flag here, since #1940 cannot be either
    cited issue's fragment."""
    root = _repo(tmp_path, {
        "1940.md": "### Added — Entry (#1848, #1814)\n\nBody.",
    })
    problems = gate.find_problems(root)
    assert len(problems) == 1
    assert "1940.md" in problems[0]
    assert "#1848" in problems[0]
    assert "#1814" in problems[0]


def test_a_heading_repeating_the_same_number_is_still_checked(tmp_path: Path) -> None:
    """changelog.d/1818.md's shape: `#1818` cited twice in the heading --
    same value both times, so it collapses to one distinct citation and is
    still enforced (not treated as an ambiguous multi-issue heading)."""
    clean_root = tmp_path / "clean"
    clean_root.mkdir()
    root = _repo(clean_root, {
        "1818.md": "### Reverted — a #1818 follow-up regressed something (#1818)\n\nBody.",
    })
    assert gate.find_problems(root) == []

    mismatched_root = tmp_path / "mismatched"
    mismatched_root.mkdir()
    mismatched = _repo(mismatched_root, {
        "1819.md": "### Reverted — a #1818 follow-up regressed something (#1818)\n\nBody.",
    })
    problems = gate.find_problems(mismatched)
    assert len(problems) == 1
    assert "1819.md" in problems[0]


def test_body_only_citations_are_never_scoped(tmp_path: Path) -> None:
    """Only the heading (first non-blank line) is scoped -- a body citing an
    issue is not this gate's concern, even when the heading itself cites
    none. A whole-file scan (rather than heading-only) would pick up the
    body's `#999`, treat it as the sole citation, and wrongly flag this
    against the filename's leading `853` -- so this fixture, unlike a
    heading that already cites its own filename number, actually
    discriminates a whole-file-scan mutant from the real heading-only
    scope."""
    root = _repo(tmp_path, {
        "853.md": "### Changed\n\n- Documented something, fixes #999.",
    })
    assert gate.find_problems(root) == []


def test_a_leading_blank_line_before_the_heading_is_skipped(tmp_path: Path) -> None:
    """`_heading_line`'s contract is the first NON-BLANK line, not the first
    line outright. Mutating that to always return the literal first line
    would return the blank line here, fail the `### ` prefix check, and
    silently skip a fragment whose heading actually disagrees with its
    filename -- so this fixture only passes when the blank line is genuinely
    stepped over."""
    root = _repo(tmp_path, {
        "902.md": "\n### Added — Entry (#901)\n\nBody.",
    })
    problems = gate.find_problems(root)
    assert len(problems) == 1
    assert "902.md" in problems[0]


def test_an_alp_sdk_prefixed_citation_is_not_recognized(tmp_path: Path) -> None:
    """`_HEADING_ISSUE_RE` requires a non-word character immediately before
    `#` -- the repo-common `alp-sdk#N` form has a `k` there, so it is NOT
    recognized as a citation at all, even though the heading's number
    disagrees with the filename. This pins that as current, documented
    behaviour: dropping the `(?<!\\w)` guard would make this heading's
    `#901` citation visible, disagree with the `902.md` filename, and flag
    it -- so this fixture discriminates that mutation."""
    root = _repo(tmp_path, {
        "902.md": "### Added — Entry (alp-sdk#901)\n\nBody.",
    })
    assert gate.find_problems(root) == []


def test_no_fragments_at_all_passes(tmp_path: Path) -> None:
    assert gate.find_problems(_repo(tmp_path, {})) == []


def test_main_exits_nonzero_on_the_real_gate_when_problems_exist(tmp_path: Path, monkeypatch, capsys) -> None:
    root = _repo(tmp_path, {
        "301.md": "### Added — Entry (#302)\n\nBody.",
    })
    monkeypatch.setattr(sys, "argv", ["check_changelog_fragment_issue.py", "--root", str(root)])
    rc = gate.main()
    assert rc == 1
    assert "301.md" in capsys.readouterr().err
