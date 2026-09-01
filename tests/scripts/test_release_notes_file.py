# SPDX-License-Identifier: Apache-2.0
"""
Regression tests for issue #1728: `.github/workflows/release.yml`'s
"Prefer the committed release-notes file for the release body" step
(`id: release_notes`) mechanises `cutting-a-release`'s step 0 -- the lean
Release-page body used to be drafted into a local scratch file and pasted
in *after* the tag, via `gh release edit v<N> --notes-file <lean.md>`,
leaving the raw CHANGELOG auto-slice as the public body until a human
noticed and fixed it. It's now `docs/release-notes/v<CORE_VERSION>.md`,
committed to the repo *before* the tag, and this step reads it directly.

These tests run the WORKFLOW'S OWN `run:` script -- extracted from the
YAML, not retyped here -- against a constructed working directory, so a
regression in the real step (not a copy of it) turns them red:

  1. A final tag with the notes file committed: `release_notes.md` (the
     changelog-slice step's output) is overwritten with the file's exact
     content.
  2. A final tag with NO notes file: the job fails loudly (`::error::`,
     nonzero exit) instead of publishing the CHANGELOG draft, and
     `release_notes.md` is left untouched.
  3. A final tag with an EMPTY notes file: same fail-loud behaviour --
     an empty committed file is not "no file yet", it's a forgotten step.
  4. A pre-release (`-rcN`) tag with no notes file yet (the exact
     scenario an rc is always cut in): falls back to whatever the
     changelog-slice step already wrote to `release_notes.md`,
     untouched, exit 0.
  5. A pre-release tag WITH a notes file already committed: still
     prefers it over the CHANGELOG draft (the file, once written, is
     always more accurate than a slice).

Run against pre-fix `release.yml` (no `id: release_notes` step at all),
`_extract_workflow_run_step` itself raises `AssertionError` -- there is
nothing here for this suite to pass against.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
RELEASE_WORKFLOW = REPO / ".github" / "workflows" / "release.yml"

pytestmark_bash = pytest.mark.skipif(
    sys.platform.startswith("win"),
    reason="the release.yml step is a POSIX bash run: block; run this "
    "test on Linux/macOS/WSL.",
)


def _extract_workflow_run_step(workflow_path: Path, job: str, step_id: str) -> str:
    """Return the `run:` script body of the `jobs.<job>.steps` entry with
    the given `id`, parsed out of the workflow YAML -- so a test executes
    the WORKFLOW'S OWN text instead of a hand-typed copy of it (a copy can
    drift from the workflow and still pass)."""
    doc = yaml.safe_load(workflow_path.read_text(encoding="utf-8"))
    for step in doc["jobs"][job]["steps"]:
        if step.get("id") == step_id:
            return step["run"]
    raise AssertionError(f"no step id={step_id!r} in {job!r} of {workflow_path}")


def _run_release_notes_step(
    tmp_path: Path,
    version: str,
    is_prerelease: bool,
    *,
    notes_content: str | None,
    existing_release_notes: str = "STALE CHANGELOG DRAFT -- must be overwritten",
) -> subprocess.CompletedProcess:
    """Run the real `id: release_notes` step. `VERSION`/`IS_PRERELEASE`
    are assigned via workflow expressions substituted by GitHub before
    bash ever sees the script -- reproduce that substitution here, same
    as the sibling `id: changelog` step's own test does.

    `existing_release_notes` seeds `release_notes.md` as the preceding
    `id: changelog` step would have left it, so a pass here proves the
    step actually OVERWRITES/PRESERVES it correctly rather than merely
    producing SOME output. `notes_content=None` means no
    `docs/release-notes/v<version>.md` file is committed at all."""
    run_script = _extract_workflow_run_step(RELEASE_WORKFLOW, "build", "release_notes")
    run_script = run_script.replace(
        "${{ steps.tag.outputs.core_version }}", version
    ).replace(
        "${{ steps.tag.outputs.is_prerelease }}", "true" if is_prerelease else "false"
    )
    (tmp_path / "release_notes.md").write_text(existing_release_notes, encoding="utf-8")
    notes_dir = tmp_path / "docs" / "release-notes"
    notes_dir.mkdir(parents=True)
    if notes_content is not None:
        (notes_dir / f"v{version}.md").write_text(notes_content, encoding="utf-8")
    return subprocess.run(
        ["bash", "-c", run_script],
        cwd=str(tmp_path),
        capture_output=True,
        text=True,
        timeout=30,
    )


def _release_notes(tmp_path: Path) -> str:
    return (tmp_path / "release_notes.md").read_text(encoding="utf-8")


@pytestmark_bash
def test_final_tag_with_notes_file_overwrites_the_draft(tmp_path):
    lean_body = "## Highlights\n\n- **Something shipped.**\n"
    proc = _run_release_notes_step(tmp_path, "1.2.3", False, notes_content=lean_body)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert _release_notes(tmp_path) == lean_body


@pytestmark_bash
def test_final_tag_with_no_notes_file_fails_the_job(tmp_path):
    proc = _run_release_notes_step(tmp_path, "1.2.3", False, notes_content=None)
    assert proc.returncode != 0, (
        "a final tag with no committed release-notes file must fail the "
        f"release, not fall back to the CHANGELOG draft: {proc.stdout + proc.stderr}"
    )
    assert "::error::" in proc.stderr
    assert "docs/release-notes/v1.2.3.md" in proc.stderr
    assert _release_notes(tmp_path) == "STALE CHANGELOG DRAFT -- must be overwritten", (
        "release_notes.md must be untouched on failure"
    )


@pytestmark_bash
def test_final_tag_with_empty_notes_file_fails_the_job(tmp_path):
    proc = _run_release_notes_step(tmp_path, "1.2.3", False, notes_content="")
    assert proc.returncode != 0, (
        "an empty committed notes file is a forgotten step, not an "
        f"absent one -- must fail: {proc.stdout + proc.stderr}"
    )
    assert "::error::" in proc.stderr
    assert _release_notes(tmp_path) == "STALE CHANGELOG DRAFT -- must be overwritten"


@pytestmark_bash
def test_prerelease_tag_with_no_notes_file_keeps_the_changelog_draft(tmp_path):
    # `version` here mirrors `steps.tag.outputs.core_version` -- the "Parse
    # tag" step strips any `-rcN` suffix before setting that output, so an
    # rc1 tag of v1.2.3 still resolves to the CORE version "1.2.3" (see
    # that step's `CORE_VERSION="${VERSION%%-*}"`), never "1.2.3-rc1".
    proc = _run_release_notes_step(tmp_path, "1.2.3", True, notes_content=None)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert _release_notes(tmp_path) == "STALE CHANGELOG DRAFT -- must be overwritten"


@pytestmark_bash
def test_prerelease_tag_with_notes_file_still_prefers_it(tmp_path):
    lean_body = "## Highlights\n\n- **Early build.**\n"
    proc = _run_release_notes_step(
        tmp_path, "1.2.3", True, notes_content=lean_body
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert _release_notes(tmp_path) == lean_body
