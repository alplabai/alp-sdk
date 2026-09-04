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
    monkeypatch.setattr(module, "CHANGELOG_D", tmp_path / "changelog.d")
    monkeypatch.setattr(module, "VERSION_H", tmp_path / "version.h")
    monkeypatch.setattr(module, "PYPROJECT", tmp_path / "pyproject.toml")
    monkeypatch.setattr(module, "BANNER_C", tmp_path / "alp_banner.c")
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


def test_slicing_with_pending_changelog_fragments_is_refused(bv):
    """#1395: an unfolded fragment must block the slice, not silently ship
    with an empty [Unreleased] section on the next cycle."""
    bv.CHANGELOG.write_text(
        "# Changelog\n\n"
        "## [Unreleased] - v0.16.0 candidate\n\n"
        "### Added — Existing entry\n\n"
        "body\n\n"
        "## [v0.15.0] - 2026-08-07\n\n"
        "what actually shipped\n",
        encoding="utf-8",
    )
    before = bv.CHANGELOG.read_text(encoding="utf-8")
    bv.CHANGELOG_D.mkdir()
    (bv.CHANGELOG_D / "README.md").write_text("contract\n", encoding="utf-8")
    (bv.CHANGELOG_D / "1358.md").write_text(
        "### Fixed — Would be silently dropped\n\nbody\n", encoding="utf-8"
    )

    with pytest.raises(SystemExit) as excinfo:
        bv.slice_changelog("0.16.0", dry_run=False)

    assert "1358.md" in str(excinfo.value)
    assert "assemble_changelog.py" in str(excinfo.value)
    # The refusal must leave CHANGELOG.md untouched.
    assert bv.CHANGELOG.read_text(encoding="utf-8") == before
    # ... and must not have consumed the fragment either.
    assert (bv.CHANGELOG_D / "1358.md").is_file()


def test_slicing_with_only_readme_in_changelog_d_is_not_blocked(bv):
    """README.md is the contract doc, not a pending fragment."""
    bv.CHANGELOG.write_text(
        "# Changelog\n\n"
        "## [Unreleased] - v0.16.0 candidate\n\n"
        "### Added — Existing entry\n\n"
        "body\n",
        encoding="utf-8",
    )
    bv.CHANGELOG_D.mkdir()
    (bv.CHANGELOG_D / "README.md").write_text("contract\n", encoding="utf-8")

    bv.slice_changelog("0.16.0", dry_run=False)

    assert "## [v0.16.0]" in bv.CHANGELOG.read_text(encoding="utf-8")


def test_a_full_bump_regenerates_the_emit_goldens_too(bv, monkeypatch, capsys):
    """#1461: build-plan's `sdkVersion` and a released scaffold's README
    doc links both bake in metadata/sdk_version.yaml's version, so a bump
    must refresh tests/fixtures/emit-snapshots/ the same way it already
    refreshes the ABI snapshot -- not leave a human to discover the drift
    the next time check_emit_snapshots.py runs.

    Regression-proof: comment out the `regenerate_emit_snapshots(...)`
    call in `main()` and this test fails (no recorded --update call).
    """
    bv.SDK_VERSION_YAML.write_text("version: 0.15.0\nstatus:  released\n", encoding="utf-8")
    bv.CHANGELOG.write_text(
        "# Changelog\n\n## [Unreleased] - v0.16.0 candidate\n\nbody\n", encoding="utf-8"
    )
    # main() also rewrites these three -- point them at scratch files so
    # the test can't touch the real repo checkout.
    version_h = bv.REPO / "version.h"
    version_h.write_text(
        "#define ALP_VERSION_MAJOR 0\n"
        "#define ALP_VERSION_MINOR 15\n"
        "#define ALP_VERSION_PATCH 0\n"
        '#define ALP_VERSION_STRING "0.15.0"\n',
        encoding="utf-8",
    )
    monkeypatch.setattr(bv, "VERSION_H", version_h)
    pyproject = bv.REPO / "pyproject.toml"
    pyproject.write_text('[project]\nversion = "0.15.0"\n', encoding="utf-8")
    monkeypatch.setattr(bv, "PYPROJECT", pyproject)
    banner_c = bv.REPO / "alp_banner.c"
    banner_c.write_text("/* Alp SDK 0.15.0 */\n", encoding="utf-8")
    monkeypatch.setattr(bv, "BANNER_C", banner_c)
    # regenerate_abi_snapshot() only prints a REPO-relative path after the
    # (mocked) subprocess call -- ABI_DIR was computed at import time
    # against the real REPO, so it must move too.
    monkeypatch.setattr(bv, "ABI_DIR", bv.REPO / "docs" / "abi")

    calls: list[list[str]] = []
    monkeypatch.setattr(bv.subprocess, "check_call", lambda cmd: calls.append(list(cmd)))
    monkeypatch.setattr(sys, "argv", ["bump_version.py", "--to", "0.16.0"])

    rc = bv.main()

    assert rc == 0
    emit_calls = [c for c in calls if str(bv.EMIT_SNAPSHOT_TOOL) in c]
    assert len(emit_calls) == 1, f"expected exactly one --emit-goldens refresh, got: {calls}"
    assert "--update" in emit_calls[0]


def test_the_repo_changelog_has_exactly_one_heading_per_version():
    """The real CHANGELOG, not a fixture -- this is what release.yml slices."""
    import collections
    import re

    text = (REPO / "CHANGELOG.md").read_text(encoding="utf-8")
    headings = re.findall(r"^## \[(v[^\]]+)\]", text, re.MULTILINE)
    duplicates = [v for v, n in collections.Counter(headings).items() if n > 1]

    assert not duplicates, f"duplicate CHANGELOG version headings: {duplicates}"


# ---------------------------------------------------------------------
# #1902: a pre-release bump (`--to X.Y.Z-rcN`) must make the checkout
# identify itself as an rc, not silently declare its eventual GA version.
# ---------------------------------------------------------------------


def test_prerelease_bump_skips_the_changelog_slice(bv):
    """An rc has no GA CHANGELOG section of its own yet -- slicing one would
    starve release.yml's documented `## [Unreleased]` pre-release fallback
    of any content (it only ever searches for the CORE version's heading)."""
    bv.CHANGELOG.write_text(
        "# Changelog\n\n"
        "## [Unreleased] - v0.16.0 candidate\n\n"
        "work in progress\n",
        encoding="utf-8",
    )
    before = bv.CHANGELOG.read_text(encoding="utf-8")

    bv.slice_changelog("0.16.0-rc1", dry_run=False)

    assert bv.CHANGELOG.read_text(encoding="utf-8") == before
    assert "## [v0.16.0-rc1]" not in before  # sanity: really wasn't sliced


def test_ga_bump_still_slices_the_changelog(bv):
    """Regression: a bare (non-prerelease) `--to` must be unaffected."""
    bv.CHANGELOG.write_text(
        "# Changelog\n\n"
        "## [Unreleased] - v0.16.0 candidate\n\n"
        "work in progress\n",
        encoding="utf-8",
    )

    bv.slice_changelog("0.16.0", dry_run=False)

    assert "## [v0.16.0]" in bv.CHANGELOG.read_text(encoding="utf-8")


def test_prerelease_bump_writes_the_full_string_to_version_h(bv):
    """ALP_VERSION_STRING carries the suffix; MAJOR/MINOR/PATCH stay the
    plain core integers -- this is the actual self-identification fix."""
    bv.VERSION_H.write_text(
        '#define ALP_VERSION_MAJOR 0\n'
        '#define ALP_VERSION_MINOR 15\n'
        '#define ALP_VERSION_PATCH 0\n'
        '#define ALP_VERSION_STRING "0.15.0"\n',
        encoding="utf-8",
    )

    bv.update_version_h("0.16.0-rc1", dry_run=False)

    text = bv.VERSION_H.read_text(encoding="utf-8")
    assert "#define ALP_VERSION_MAJOR 0" in text
    assert "#define ALP_VERSION_MINOR 16" in text
    assert "#define ALP_VERSION_PATCH 0" in text
    assert '#define ALP_VERSION_STRING "0.16.0-rc1"' in text


def test_prerelease_bump_pins_pyproject_and_banner_to_the_core_version(bv):
    """PEP 440 doesn't accept a bare "-rc1" suffix, and neither file is the
    self-identification surface -- both stay pinned to the core triple,
    unlike version.h's ALP_VERSION_STRING above."""
    bv.PYPROJECT.write_text('[project]\nname = "alp-sdk-cli"\nversion = "0.15.0"\n',
                             encoding="utf-8")
    bv.BANNER_C.write_text(
        "/*\n * Sample banner:\n *\n"
        " *   Alp SDK 0.15.0  |  E1M-AEN801  |  (c) Alp Lab AB\n */\n",
        encoding="utf-8",
    )

    bv.update_pyproject("0.16.0-rc1", dry_run=False)
    bv.update_banner_c("0.16.0-rc1", dry_run=False)

    assert 'version = "0.16.0"' in bv.PYPROJECT.read_text(encoding="utf-8")
    assert "-rc1" not in bv.PYPROJECT.read_text(encoding="utf-8")
    assert "Alp SDK 0.16.0" in bv.BANNER_C.read_text(encoding="utf-8")
    assert "-rc1" not in bv.BANNER_C.read_text(encoding="utf-8")


def test_ga_bump_after_an_rc_slices_the_changelog_normally(bv):
    """The two-step rc-then-GA workflow end to end: the rc bump leaves
    [Unreleased] open (previous test), and the LATER bare bump does the
    real slice with no leftover rc heading in the way."""
    bv.CHANGELOG.write_text(
        "# Changelog\n\n"
        "## [Unreleased] - v0.16.0 candidate\n\n"
        "work in progress\n",
        encoding="utf-8",
    )
    bv.slice_changelog("0.16.0-rc1", dry_run=False)  # rc: no-op, per above

    bv.slice_changelog("0.16.0", dry_run=False)  # later GA bump: real slice

    text = bv.CHANGELOG.read_text(encoding="utf-8")
    assert "## [v0.16.0]" in text
    assert "## [v0.16.0-rc1]" not in text  # never created
