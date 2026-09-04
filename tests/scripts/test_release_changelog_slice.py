# SPDX-License-Identifier: Apache-2.0
"""
Regression tests for issue #1492: `.github/workflows/release.yml`'s
"Verify + slice CHANGELOG section for the release notes" step (`id:
changelog`) published the *entire* CHANGELOG section as the GitHub
Release body -- 255,413 chars for `v0.16.0-rc1` -- with no size cap,
relying on `softprops/action-gh-release`'s undocumented, unwarned
`substring(0, 124999)` to keep it under GitHub's 125,000-char limit.

The first fix for #1492 added a boundary-aware truncation (cut at the
last complete `### ` CHANGELOG entry below the budget) but introduced
a worse bug than the one it replaced: `str.rfind("\n### ", 0, budget)`
returns the LAST match below budget with no floor, so when the first
CHANGELOG entry alone overruns the budget, the match it finds is the
one at the very start of the body (or the pre-release prefix) --
publishing a near-empty release body with `rc=0` and no error. It also
didn't track fenced-code-block state, so a `### `-looking line inside
an open ` ``` ` block could be picked as the cut point, publishing an
unclosed fence.

These tests run the WORKFLOW'S OWN `run:` script -- extracted from the
YAML, not retyped here -- against constructed CHANGELOG.md fixtures,
so a regression in the real step (not a copy of it) turns them red:

  1. A body under the cap passes through byte-identical.
  2. A body over the cap, with a real entry boundary that keeps a
     majority of the budget, truncates there and warns.
  3. A body over the cap with NO safe boundary below the limit (the
     first entry alone is oversized) FAILS the job instead of
     publishing a near-empty body -- the #1492 blocker's own repro.
  4. A `### `-looking line inside an open ``` fence is never chosen as
     the cut point; the real boundary before the fence is used
     instead, so the truncated body never contains an unclosed fence.
  5. The pre-existing missing-heading `exit 1` path is unaffected.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
RELEASE_WORKFLOW = REPO / ".github" / "workflows" / "release.yml"

pytestmark_bash = pytest.mark.skipif(
    sys.platform.startswith("win"),
    reason="the release.yml step is a POSIX bash+python run: block; run "
    "this test on Linux/macOS/WSL.",
)


def _extract_workflow_run_step(workflow_path: Path, job: str, step_id: str) -> str:
    """Return the `run:` script body of the `jobs.<job>.steps` entry
    with the given `id`, parsed out of the workflow YAML -- so a test
    can execute the WORKFLOW'S OWN text instead of a hand-typed copy
    of it (a copy can drift from the workflow and still pass)."""
    doc = yaml.safe_load(workflow_path.read_text(encoding="utf-8"))
    for step in doc["jobs"][job]["steps"]:
        if step.get("id") == step_id:
            return step["run"]
    raise AssertionError(f"no step id={step_id!r} in {job!r} of {workflow_path}")


def _run_changelog_step(
    tmp_path: Path, changelog_text: str, version: str, is_prerelease: bool
) -> subprocess.CompletedProcess:
    """Run the real `id: changelog` step against a fixture CHANGELOG.md.
    `VERSION`/`IS_PRERELEASE` are assigned in the step via workflow
    expressions (`${{ steps.tag.outputs.* }}`), substituted by GitHub
    before bash ever sees the script -- reproduce that substitution
    here so the extracted text runs unmodified otherwise.

    The step's own last line redirects the body to `release_notes.md`
    (`... <<'PY' > release_notes.md`), not stdout -- `proc.stdout`
    stays empty on success; callers read `tmp_path/release_notes.md`."""
    run_script = _extract_workflow_run_step(RELEASE_WORKFLOW, "build", "changelog")
    run_script = run_script.replace(
        "${{ steps.tag.outputs.core_version }}", version
    ).replace(
        "${{ steps.tag.outputs.is_prerelease }}", "true" if is_prerelease else "false"
    )
    (tmp_path / "CHANGELOG.md").write_text(changelog_text, encoding="utf-8")
    return subprocess.run(
        ["bash", "-c", run_script],
        cwd=str(tmp_path),
        capture_output=True,
        text=True,
        timeout=30,
    )


def _release_notes(tmp_path: Path) -> str:
    notes = tmp_path / "release_notes.md"
    return notes.read_text(encoding="utf-8") if notes.exists() else ""


def _entry(heading: str, filler_char: str, chars: int) -> str:
    """One `### <heading>` CHANGELOG entry with `chars` filler chars
    wrapped at 79 columns, so it looks like real prose rather than one
    giant unbroken line."""
    body_lines = [filler_char * 79 for _ in range(chars // 79)]
    return f"### {heading}\n\n" + "\n".join(body_lines) + "\n"


@pytestmark_bash
def test_under_cap_body_passes_through_byte_identical(tmp_path):
    """A body well under GitHub's 125,000-char limit is untouched: no
    truncation note, no ::warning::, exact CHANGELOG content out."""
    entry = "### Added\n\n- a small, unremarkable change.\n"
    changelog = f"# Changelog\n\n## [v1.0.0] - 2026-01-01\n\n{entry}"
    proc = _run_changelog_step(tmp_path, changelog, "1.0.0", False)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    notes = _release_notes(tmp_path)
    assert notes.strip() == entry.strip()
    assert "::warning::" not in proc.stderr
    assert "truncated" not in notes


@pytestmark_bash
def test_over_cap_with_usable_boundary_truncates_there_and_warns(tmp_path):
    """Two entries; the first alone is big but leaves more than half
    the budget, so the cut lands at the *second* entry's boundary
    (dropping it whole) rather than mid-word. The ::warning:: names
    both the original AND the truncated length (#1492 round-1 finding
    4: only the limit was named before, never the truncated size)."""
    first = _entry("Added -- first, big", "a", 90_000)
    second = _entry("Fixed -- second, big", "b", 60_000)
    changelog = f"# Changelog\n\n## [v1.0.0] - 2026-01-01\n\n{first}\n{second}"
    proc = _run_changelog_step(tmp_path, changelog, "1.0.0", False)
    assert proc.returncode == 0, proc.stdout + proc.stderr

    out = _release_notes(tmp_path)
    assert "Fixed -- second, big" not in out, "second entry should be dropped whole"
    assert "> ... truncated:" in out
    assert len(out) < 125_000

    warning = proc.stderr
    m = re.search(
        r"CHANGELOG slice is (\d+) chars.*Truncated the DRAFT body to (\d+) chars",
        warning,
        re.DOTALL,
    )
    assert m, f"warning did not name both lengths: {warning!r}"
    original_len, truncated_len = int(m.group(1)), int(m.group(2))
    assert original_len > 125_000
    assert truncated_len == len(out.rstrip("\n"))
    assert truncated_len < original_len


@pytestmark_bash
def test_over_cap_with_no_usable_boundary_fails_the_job(tmp_path):
    """The #1492 round-1 blocker repro: a SINGLE CHANGELOG entry alone
    overruns the budget. The old code's unfloored `rfind` picked the
    boundary at the very start of the body and published a near-empty
    release with `rc=0`. The fix must fail the job instead."""
    entry = _entry("Added -- the only, giant entry", "x", 240_000)
    changelog = f"# Changelog\n\n## [v1.0.0] - 2026-01-01\n\n{entry}"
    proc = _run_changelog_step(tmp_path, changelog, "1.0.0", False)
    assert proc.returncode != 0, (
        "must fail rather than publish a near-empty body: "
        f"stdout={proc.stdout!r} stderr={proc.stderr!r}"
    )
    assert "::error::" in proc.stderr
    assert _release_notes(tmp_path).strip() == "", (
        "no body should reach release_notes.md on failure"
    )


@pytestmark_bash
def test_over_cap_boundary_inside_fence_is_skipped(tmp_path):
    """A `### `-looking line that sits inside an OPEN ``` fence must
    never be chosen as the cut point -- it would publish a body with
    an unclosed fence and swallow the truncation note inside it. The
    real (pre-fence) boundary is used instead, and the truncated
    output has a balanced number of ``` fence markers."""
    first = _entry("Added -- one", "z", 90_000)
    second_with_fence = (
        "### Added -- two\n\n"
        + "\n".join("z" * 79 for _ in range(400))
        + "\n\n```sh\n"
        + "### not a heading\n"
        + "\n".join("z" * 79 for _ in range(400))
        + "\n```\n"
    )
    changelog = (
        f"# Changelog\n\n## [v1.0.0] - 2026-01-01\n\n{first}\n{second_with_fence}"
    )
    proc = _run_changelog_step(tmp_path, changelog, "1.0.0", False)
    assert proc.returncode == 0, proc.stdout + proc.stderr

    out = _release_notes(tmp_path)
    assert "### not a heading" not in out, (
        "the boundary inside the fence must never be selected as the cut point"
    )
    assert out.count("```") % 2 == 0, f"unclosed fence in truncated output: {out!r}"


@pytestmark_bash
def test_missing_heading_still_fails_cleanly(tmp_path):
    """Pre-existing behaviour (unrelated to the #1492 size guard) must
    survive untouched: tagging a version with no matching CHANGELOG
    heading fails loudly rather than publishing a stub body."""
    changelog = "# Changelog\n\n## [Unreleased]\n\n### Added\n- something\n"
    proc = _run_changelog_step(tmp_path, changelog, "9.9.9", False)
    assert proc.returncode != 0
    assert "::error::" in proc.stderr
    assert "## [v9.9.9]" in proc.stderr
