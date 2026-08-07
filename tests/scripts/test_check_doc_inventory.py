# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_doc_inventory.py.

Issue #1265: doc-stated workflow names/counts and ADR/chip/library counts
must match the tree.  These tests lock down that a missing doc or a
changed anchor is a FAILURE, never a silent pass; that every occurrence
of a count claim is checked, not just the first one `re.search()` would
find; and that the gate does NOT flag the legitimate historical/other-repo
`*.yml` mentions its own docstring calls out.

Run locally:

    python -m pytest tests/scripts/test_check_doc_inventory.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_doc_inventory as gate  # noqa: E402


def _scaffold(root: Path) -> None:
    (root / ".github" / "workflows").mkdir(parents=True, exist_ok=True)
    (root / ".github" / "workflows" / "pr-twister.yml").write_text("name: pr-twister\n", encoding="utf-8")
    (root / ".github" / "workflows" / "pr-doxygen.yml").write_text("name: pr-doxygen\n", encoding="utf-8")

    ci_dir = root / "docs" / "ci"
    ci_dir.mkdir(parents=True, exist_ok=True)
    (ci_dir / "README.md").write_text(
        "## Workflows shipped\n\n"
        "`.github/workflows/` carries **2** workflow files as of this revision.\n\n"
        "| Workflow | Trigger |\n"
        "|---|---|\n"
        "| [`pr-twister.yml`](../../.github/workflows/pr-twister.yml) | every PR |\n"
        "| [`pr-doxygen.yml`](../../.github/workflows/pr-doxygen.yml) | every PR |\n\n"
        "## Workflows planned\n\n"
        "| Workflow | Target |\n"
        "|---|---|\n"
        "| `release-abi-snapshot.yml` | v1.0 |\n\n"
        "(History: this repo shipped a `retired-example.yml` skeleton workflow "
        "and the workflow was deleted.)\n",
        encoding="utf-8",
    )

    (root / "docs").mkdir(parents=True, exist_ok=True)
    (root / "docs" / "README.md").write_text(
        "- [adr/README.md](adr/README.md) — Architecture Decision Records "
        "index (2 ADRs; recount with `ls docs/adr/[0-9]*.md | wc -l`).\n",
        encoding="utf-8",
    )
    adr_dir = root / "docs" / "adr"
    adr_dir.mkdir(parents=True, exist_ok=True)
    (adr_dir / "0001-example.md").write_text("# ADR 1\n", encoding="utf-8")
    (adr_dir / "0002-example.md").write_text("# ADR 2\n", encoding="utf-8")
    (adr_dir / "README.md").write_text("# ADR index\n", encoding="utf-8")

    (root / "docs" / "contributing-tier-2.md").write_text(
        "Tier 1: 2 chips, 1 libraries.  Maintainer-curated.\n", encoding="utf-8",
    )
    for name in ("foo", "bar"):
        (root / "chips" / name).mkdir(parents=True, exist_ok=True)
    libraries_dir = root / "metadata" / "libraries"
    libraries_dir.mkdir(parents=True, exist_ok=True)
    (libraries_dir / "coap.yaml").write_text("name: coap\n", encoding="utf-8")


def test_clean_tree_passes(tmp_path: Path):
    _scaffold(tmp_path)
    assert gate.find_problems(tmp_path) == []


def test_workflow_count_drift_fails(tmp_path: Path):
    _scaffold(tmp_path)
    doc = tmp_path / "docs" / "ci" / "README.md"
    doc.write_text(doc.read_text(encoding="utf-8").replace(
        "carries **2** workflow files", "carries **3** workflow files"), encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert any("states **3**" in p and "has 2" in p for p in problems)


def test_yaml_extension_workflow_counted_too(tmp_path: Path):
    """The real-count glob must cover *.yaml, not only *.yml, or a .yaml
    workflow silently escapes the count."""
    _scaffold(tmp_path)
    (tmp_path / ".github" / "workflows" / "pr-extra.yaml").write_text("name: extra\n", encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert any("has 3" in p for p in problems)


def test_workflow_count_checks_every_occurrence_not_just_first(tmp_path: Path):
    """re.search() picking only the FIRST match let an accurate decoy
    occurrence shadow a stale one later in the same doc -- every
    occurrence of the anchor must be checked independently."""
    _scaffold(tmp_path)
    doc = tmp_path / "docs" / "ci" / "README.md"
    original = doc.read_text(encoding="utf-8")
    accurate_decoy = "(Also noted elsewhere: carries **2** workflow files.)\n\n"
    stale = original.replace("carries **2** workflow files", "carries **9** workflow files")
    doc.write_text(accurate_decoy + stale, encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert any("states **9**" in p and "has 2" in p for p in problems)


def test_workflow_link_text_href_mismatch_fails(tmp_path: Path):
    _scaffold(tmp_path)
    doc = tmp_path / "docs" / "ci" / "README.md"
    doc.write_text(doc.read_text(encoding="utf-8").replace(
        "[`pr-twister.yml`](../../.github/workflows/pr-twister.yml)",
        "[`pr-twister-OLD.yml`](../../.github/workflows/pr-twister.yml)"), encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert any("link text `pr-twister-OLD.yml`" in p for p in problems)


def test_workflow_link_with_line_fragment_not_flagged(tmp_path: Path):
    """A deep link like '...pr-twister.yml#L20' names a real file plus a
    line anchor -- Path(href).name without stripping the fragment would
    read the whole '#L20' suffix as part of the filename and falsely
    flag a text/filename mismatch."""
    _scaffold(tmp_path)
    doc = tmp_path / "docs" / "ci" / "README.md"
    doc.write_text(doc.read_text(encoding="utf-8").replace(
        "[`pr-twister.yml`](../../.github/workflows/pr-twister.yml)",
        "[`pr-twister.yml`](../../.github/workflows/pr-twister.yml#L20)"), encoding="utf-8")
    assert gate.find_problems(tmp_path) == []


def test_dead_workflow_link_target_not_flagged_here(tmp_path: Path):
    """Link-TARGET existence is scripts/check_doc_links.py's job (it
    already scans docs/ci/** for dead relative paths in the same
    pr-doc-drift.yml job) -- this gate only checks link text vs. the
    href's own filename (both renamed here in lockstep, so that check
    still agrees), so a renamed/missing target is out of scope here and
    must not be double-reported."""
    _scaffold(tmp_path)
    doc = tmp_path / "docs" / "ci" / "README.md"
    doc.write_text(doc.read_text(encoding="utf-8").replace(
        "[`pr-twister.yml`](../../.github/workflows/pr-twister.yml)",
        "[`pr-twister-gone.yml`](../../.github/workflows/pr-twister-gone.yml)"), encoding="utf-8")
    assert gate.find_problems(tmp_path) == []


def test_planned_table_entries_are_not_link_checked(tmp_path: Path):
    """`release-abi-snapshot.yml` in '## Workflows planned' has no link
    syntax (the workflow doesn't exist yet) -- must never be flagged, but
    the gate must still be alive: an unrelated real drift introduced in
    the same tree is still caught, proving the pass isn't just a dead
    check."""
    _scaffold(tmp_path)
    doc = tmp_path / "docs" / "ci" / "README.md"
    doc.write_text(doc.read_text(encoding="utf-8").replace(
        "carries **2** workflow files", "carries **3** workflow files"), encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert not any("release-abi-snapshot" in p for p in problems)
    assert any("states **3**" in p for p in problems)


def test_historical_retired_workflow_mention_is_not_flagged(tmp_path: Path):
    """The nightly-aen-hil.yml counter-example from the real tree: a plain
    backtick filename in prose (no markdown-link syntax) naming a workflow
    that no longer exists must NOT be flagged -- this gate only checks
    filenames that appear inside a real [`name`](href) link."""
    _scaffold(tmp_path)
    doc = tmp_path / "docs" / "ci" / "README.md"
    assert "retired-example.yml" in doc.read_text(encoding="utf-8")
    assert gate.find_problems(tmp_path) == []


def test_adr_count_drift_fails(tmp_path: Path):
    _scaffold(tmp_path)
    doc = tmp_path / "docs" / "README.md"
    doc.write_text(doc.read_text(encoding="utf-8").replace("(2 ADRs;", "(3 ADRs;"), encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert any("states 3 ADRs" in p and "has 2" in p for p in problems)


def test_adr_count_checks_every_occurrence_not_just_first(tmp_path: Path):
    """Same first-match-wins shadowing shape as the workflow count, on the
    ADR count anchor."""
    _scaffold(tmp_path)
    doc = tmp_path / "docs" / "README.md"
    original = doc.read_text(encoding="utf-8")
    accurate_decoy = "(2 ADRs; noted elsewhere too.)\n\n"
    stale = original.replace("(2 ADRs;", "(9 ADRs;")
    doc.write_text(accurate_decoy + stale, encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert any("states 9 ADRs" in p and "has 2" in p for p in problems)


def test_chip_count_drift_fails(tmp_path: Path):
    _scaffold(tmp_path)
    doc = tmp_path / "docs" / "contributing-tier-2.md"
    doc.write_text(doc.read_text(encoding="utf-8").replace("2 chips,", "3 chips,"), encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert any("states 3 chips" in p and "has 2" in p for p in problems)


def test_library_count_drift_fails(tmp_path: Path):
    _scaffold(tmp_path)
    doc = tmp_path / "docs" / "contributing-tier-2.md"
    doc.write_text(doc.read_text(encoding="utf-8").replace("1 libraries", "2 libraries"), encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert any("states 2 libraries" in p and "has 1" in p for p in problems)


def test_chip_library_count_checks_every_occurrence_not_just_first(tmp_path: Path):
    """Same first-match-wins shadowing shape as the workflow count, on the
    combined chip/library count anchor."""
    _scaffold(tmp_path)
    doc = tmp_path / "docs" / "contributing-tier-2.md"
    original = doc.read_text(encoding="utf-8")
    accurate_decoy = "Also: 2 chips, 1 libraries, mentioned earlier.\n\n"
    stale = original.replace("2 chips, 1 libraries", "9 chips, 5 libraries")
    doc.write_text(accurate_decoy + stale, encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert any("states 9 chips" in p and "has 2" in p for p in problems)
    assert any("states 5 libraries" in p and "has 1" in p for p in problems)


def test_missing_doc_fails_loudly_not_silently(tmp_path: Path):
    """Renaming/removing one of the four docs must be a hard failure
    naming the missing doc, never a silent `[]` pass."""
    _scaffold(tmp_path)
    (tmp_path / "docs" / "README.md").unlink()
    problems = gate.find_problems(tmp_path)
    assert any("docs/README.md" in p and "not found" in p for p in problems)


def test_missing_count_anchor_fails_loudly_not_silently(tmp_path: Path):
    _scaffold(tmp_path)
    doc = tmp_path / "docs" / "contributing-tier-2.md"
    doc.write_text("Nothing to see here.\n", encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert any("could not find" in p for p in problems)
