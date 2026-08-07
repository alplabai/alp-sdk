"""Guards on `scripts/bump_version.py` that a release cut depends on.

Both behaviours here were release-blockers on the v0.15.0 cut (#1292):

* `update_sdk_version_yaml` used to raise `SystemExit` on a no-op
  substitution, which aborted the whole bump whenever the version had
  already been written in an earlier PR -- exactly the state v0.15.0 was
  left in by ``4d0f4aae``;
* nothing refused to slice `[Unreleased]` into a `## [vX]` heading that
  already existed.  `.github/workflows/release.yml` slices the release
  body by ``## \\[v{VERSION}\\]`` and takes the FIRST match, so a second
  section publishes the new content and silently orphans the older one.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]


def _load_bump_version():
    """Import scripts/bump_version.py under its own name.

    It is a script, not a package module, so go through the spec loader
    rather than adding scripts/ to sys.path (which would shadow stdlib
    names for the rest of the session).
    """
    spec = importlib.util.spec_from_file_location(
        "_bump_version_under_test", REPO / "scripts" / "bump_version.py"
    )
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def bv(tmp_path, monkeypatch):
    """bump_version with its module-level paths redirected into tmp_path."""
    module = _load_bump_version()
    monkeypatch.setattr(module, "REPO", tmp_path)
    monkeypatch.setattr(module, "SDK_VERSION_YAML", tmp_path / "sdk_version.yaml")
    monkeypatch.setattr(module, "CHANGELOG", tmp_path / "CHANGELOG.md")
    return module


def test_an_already_current_sdk_version_yaml_does_not_abort_the_bump(bv, capsys):
    """A version bumped in an earlier PR must not block the tag's cut."""
    bv.SDK_VERSION_YAML.write_text("version: 0.15.0\nstatus:  released\n", encoding="utf-8")

    bv.update_sdk_version_yaml("0.15.0", dry_run=False)

    assert "already at version: 0.15.0" in capsys.readouterr().out
    # ... and it left the file alone rather than rewriting it identically.
    assert bv.SDK_VERSION_YAML.read_text(encoding="utf-8") == "version: 0.15.0\nstatus:  released\n"


def test_a_real_version_change_still_rewrites_the_file(bv):
    """The tolerant path must not swallow an actual bump."""
    bv.SDK_VERSION_YAML.write_text("version: 0.14.0\nstatus:  released\n", encoding="utf-8")

    bv.update_sdk_version_yaml("0.15.0", dry_run=False)

    assert "version: 0.15.0" in bv.SDK_VERSION_YAML.read_text(encoding="utf-8")


def test_slicing_over_an_existing_version_section_is_refused(bv):
    """release.yml takes the FIRST `## [vX]`, so a second one orphans the first."""
    bv.CHANGELOG.write_text(
        "# Changelog\n\n"
        "## [Unreleased] - v0.15.0 candidate\n\n"
        "new work\n\n"
        "## [v0.15.0] - 2026-07-31\n\n"
        "what actually shipped\n",
        encoding="utf-8",
    )
    before = bv.CHANGELOG.read_text(encoding="utf-8")

    with pytest.raises(SystemExit) as excinfo:
        bv.slice_changelog("0.15.0", dry_run=False)

    assert "already has a" in str(excinfo.value)
    assert "## [v0.15.0] - 2026-07-31" in str(excinfo.value)
    # The refusal must leave the file untouched -- a partially sliced
    # CHANGELOG is worse than an unsliced one.
    assert bv.CHANGELOG.read_text(encoding="utf-8") == before


def test_a_different_version_section_does_not_block_the_slice(bv):
    """Only a collision on the version being cut is a problem."""
    bv.CHANGELOG.write_text(
        "# Changelog\n\n"
        "## [Unreleased] - v0.16.0 candidate\n\n"
        "new work\n\n"
        "## [v0.15.0-rc1] - 2026-07-31\n\n"
        "the candidate\n",
        encoding="utf-8",
    )

    bv.slice_changelog("0.16.0", dry_run=False)

    text = bv.CHANGELOG.read_text(encoding="utf-8")
    assert text.count("## [v0.16.0]") == 1
    assert "## [Unreleased] - v0.17.0 candidate" in text
    # The rc1 section is a distinct heading and must survive untouched.
    assert "## [v0.15.0-rc1] - 2026-07-31" in text


def test_the_repo_changelog_has_exactly_one_heading_per_version():
    """The real CHANGELOG, not a fixture -- this is what release.yml slices."""
    import collections
    import re

    text = (REPO / "CHANGELOG.md").read_text(encoding="utf-8")
    headings = re.findall(r"^## \[(v[^\]]+)\]", text, re.MULTILINE)
    duplicates = [v for v, n in collections.Counter(headings).items() if n > 1]

    assert not duplicates, f"duplicate CHANGELOG version headings: {duplicates}"
