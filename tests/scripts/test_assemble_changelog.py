# SPDX-License-Identifier: Apache-2.0
"""Tests for `scripts/assemble_changelog.py` (#1395).

The thing under test is a release-time step that MOVES text between files
and then DELETES the source. The failure that actually costs something is
not a crash -- it is an entry that quietly does not arrive: the release
ships, the fragment is gone, and nobody notices the changelog is missing a
line until a user asks why an advertised fix is undocumented. Every test
here is aimed at that, not at pretty formatting.

alp-sdk's fragment format differs from tan-cli#676's: a fragment is already
a complete `### <Category> - <Title>` block, not a bucketed bullet under a
fixed heading, so there is no category-splicing behaviour to test here --
only concatenation order, byte-fidelity, and refusal-not-drop.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

_SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "assemble_changelog.py"
_spec = importlib.util.spec_from_file_location("assemble_changelog", _SCRIPT)
assert _spec and _spec.loader
ac = importlib.util.module_from_spec(_spec)
sys.modules["assemble_changelog"] = ac
_spec.loader.exec_module(ac)


CHANGELOG = """\
# Changelog

## [Unreleased] - v0.16.0 candidate

### Added — Pre-existing added entry

Pre-existing body text.

## [v0.15.0] - 2026-08-07

### Fixed — Shipped entry that must not move

Shipped body text.
"""


def _repo(tmp_path: Path, fragments: dict[str, str], changelog: str = CHANGELOG) -> Path:
    tmp_path.mkdir(parents=True, exist_ok=True)
    (tmp_path / "CHANGELOG.md").write_text(changelog, encoding="utf-8")
    frag = tmp_path / "changelog.d"
    frag.mkdir()
    (frag / "README.md").write_text("contract\n", encoding="utf-8")
    for name, body in fragments.items():
        (frag / name).write_text(body, encoding="utf-8")
    return tmp_path


def test_every_fragment_reaches_the_changelog_and_is_deleted(tmp_path: Path) -> None:
    """The core contract: nothing is dropped, and the source is cleaned up."""
    root = _repo(tmp_path, {
        "101.md": "### Added — Added one\n\nBody one.",
        "102.md": "### Fixed — Fixed one\n\nBody two.",
        "103.md": "### Fixed — Fixed two\n\nBody three.",
    })
    assert ac.main(["--root", str(root)]) == 0

    text = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    for expected in ("Added one", "Fixed one", "Fixed two", "Body one.", "Body two.", "Body three."):
        assert expected in text, f"{expected!r} was dropped"

    remaining = [p.name for p in (root / "changelog.d").glob("*.md") if p.name != "README.md"]
    assert remaining == [], f"fragments left behind: {remaining}"
    assert (root / "changelog.d" / "README.md").is_file(), "README.md must survive assembly"


def test_existing_entries_survive(tmp_path: Path) -> None:
    """A fragment must never overwrite hand-written text already in the section."""
    root = _repo(tmp_path, {"201.md": "### Fixed — New fixed\n\nNew body."})
    assert ac.main(["--root", str(root)]) == 0
    text = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    assert "Pre-existing added entry" in text
    assert "Pre-existing body text." in text
    assert "New fixed" in text


def test_released_sections_are_untouched(tmp_path: Path) -> None:
    """Only the Unreleased section may be edited; shipped history is immutable."""
    root = _repo(tmp_path, {"301.md": "### Fixed — New fixed\n\nNew body."})
    assert ac.main(["--root", str(root)]) == 0
    text = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    released = text.split("## [v0.15.0]", 1)[1]
    assert "New fixed" not in released, "an entry leaked into a released section"
    assert "Shipped entry that must not move" in released


def test_verbatim_technical_strings_are_not_reformatted(tmp_path: Path) -> None:
    """Register/hex/SKU strings must survive byte-for-byte -- a rewrap corrupts them."""
    body = (
        "### Fixed — Probe check\n\n"
        "DPIDR `0x4C013477`, device `AE822FA0E5597LS0_M55_HE`, I2C `0x1E`, "
        "flash_args.slot0_load_address unset (tan-cli#353)."
    )
    root = _repo(tmp_path, {"401.md": body})
    assert ac.main(["--root", str(root)]) == 0
    text = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    assert body in text, "fragment body was altered in transit"


def test_a_missing_heading_is_refused_not_dropped(tmp_path: Path) -> None:
    """The whole point: an unusable fragment must STOP the run, not vanish."""
    root = _repo(tmp_path, {"501.md": "Just prose, no heading at all."})
    assert ac.main(["--root", str(root)]) == 1
    # And nothing was consumed on the way to failing.
    assert (root / "changelog.d" / "501.md").is_file()
    assert "Just prose" not in (root / "CHANGELOG.md").read_text(encoding="utf-8")


def test_an_empty_fragment_is_refused(tmp_path: Path) -> None:
    root = _repo(tmp_path, {"601.md": "   \n"})
    assert ac.main(["--root", str(root)]) == 1
    assert (root / "changelog.d" / "601.md").is_file()


def test_a_non_numeric_filename_is_refused(tmp_path: Path) -> None:
    """Filenames outside the `<issue>.md` contract must not be silently accepted."""
    root = _repo(tmp_path, {"701.fixed.md": "### Fixed — Entry\n\nBody."})
    assert ac.main(["--root", str(root)]) == 1
    assert (root / "changelog.d" / "701.fixed.md").is_file()


def test_missing_unreleased_header_refuses_rather_than_guessing(tmp_path: Path) -> None:
    root = _repo(
        tmp_path,
        {"801.md": "### Fixed — Entry\n\nBody."},
        changelog="# Changelog\n\n## [v0.15.0] - 2026-08-07\n\n### Fixed — Old\n\nOld body.\n",
    )
    assert ac.main(["--root", str(root)]) == 1
    assert (root / "changelog.d" / "801.md").is_file()


def test_require_empty_is_a_real_gate(tmp_path: Path) -> None:
    """--require-empty must FAIL while fragments remain, or it gates nothing."""
    root = _repo(tmp_path, {"901.md": "### Fixed — Entry\n\nBody."})
    assert ac.main(["--root", str(root), "--require-empty"]) == 1
    # Fold them, then the same gate must pass -- proving it tracks real
    # state rather than always failing (or always passing).
    assert ac.main(["--root", str(root)]) == 0
    assert ac.main(["--root", str(root), "--require-empty"]) == 0


def test_assembly_is_deterministic(tmp_path: Path) -> None:
    """Same fragments -> same bytes, or every release diff is noise."""
    frags = {
        "1001.md": "### Fixed — A\n\nBody A.",
        "1002.md": "### Fixed — B\n\nBody B.",
        "1003.md": "### Added — C\n\nBody C.",
    }
    a = _repo(tmp_path / "one", frags)
    b = _repo(tmp_path / "two", frags)
    assert ac.main(["--root", str(a)]) == 0
    assert ac.main(["--root", str(b)]) == 0
    assert (a / "CHANGELOG.md").read_text(encoding="utf-8") == (
        b / "CHANGELOG.md"
    ).read_text(encoding="utf-8")


def test_assembly_order_is_numeric_by_issue_not_lexicographic(tmp_path: Path) -> None:
    """`2.md` must sort before `10.md` -- string sort would put it after."""
    root = _repo(tmp_path, {
        "10.md": "### Fixed — Ten\n\nBody ten.",
        "2.md": "### Fixed — Two\n\nBody two.",
    })
    assert ac.main(["--root", str(root)]) == 0
    text = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    assert text.index("Body two.") < text.index("Body ten.")


def test_dry_run_writes_nothing(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    root = _repo(tmp_path, {"1101.md": "### Fixed — Entry\n\nBody."})
    before = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    assert ac.main(["--root", str(root), "--dry-run"]) == 0
    assert (root / "CHANGELOG.md").read_text(encoding="utf-8") == before
    assert (root / "changelog.d" / "1101.md").is_file()
    assert "Entry" in capsys.readouterr().out


def test_no_fragments_is_a_clean_no_op(tmp_path: Path) -> None:
    root = _repo(tmp_path, {})
    before = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    assert ac.main(["--root", str(root)]) == 0
    assert (root / "CHANGELOG.md").read_text(encoding="utf-8") == before


def test_check_lists_pending_and_writes_nothing(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    root = _repo(tmp_path, {"1201.md": "### Fixed — Entry\n\nBody."})
    before = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    assert ac.main(["--root", str(root), "--check"]) == 0
    assert (root / "CHANGELOG.md").read_text(encoding="utf-8") == before
    assert (root / "changelog.d" / "1201.md").is_file()
    out = capsys.readouterr().out
    assert "1201.md" in out
    assert "1 fragment(s) pending" in out
