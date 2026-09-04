# SPDX-License-Identifier: Apache-2.0
"""
Regression tests for issue #1902: `.github/workflows/release.yml`'s
"Parse tag + verify against metadata" step (`id: tag`) is the only thing
that stops a build from self-identifying as the wrong version, and until
now it had only ever been syntax-checked (`bash -n`), never exercised.

The original defect made every rc build report itself as its eventual GA
version (fixed by stripping DECLARED's own suffix before the core
compare, plus a guard refusing a GA tag with a leftover rc suffix). Both
of those guards compare CORE versions only, so the same defect survived
in the pre-release direction: tagging `v0.16.0-rc2` against a metadata
`version: 0.16.0-rc1` build passed both checks and self-reported as rc1.
The fix adds a third guard that fires only when TAG and DECLARED both
carry a suffix and it differs.

These tests run the WORKFLOW'S OWN `run:` script -- extracted from the
YAML via the same `_extract_workflow_run_step` helper
`test_release_changelog_slice.py` uses -- against a constructed
`metadata/sdk_version.yaml` fixture and a `GITHUB_REF_NAME`, so a
regression in the real step (not a copy of it) turns these red.
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
    reason="the release.yml step is a POSIX bash+python run: block; run "
    "this test on Linux/macOS/WSL.",
)


def _extract_workflow_run_step(workflow_path: Path, job: str, step_id: str) -> str:
    """Same helper as test_release_changelog_slice.py: pull the `run:`
    body of the named step straight out of the workflow YAML so the test
    executes the WORKFLOW'S OWN text, not a hand-typed copy that could
    drift from it and still pass."""
    doc = yaml.safe_load(workflow_path.read_text(encoding="utf-8"))
    for step in doc["jobs"][job]["steps"]:
        if step.get("id") == step_id:
            return step["run"]
    raise AssertionError(f"no step id={step_id!r} in {job!r} of {workflow_path}")


def _run_tag_step(tmp_path: Path, tag: str, declared: str) -> subprocess.CompletedProcess:
    """Run the real `id: tag` step against a fixture
    metadata/sdk_version.yaml, with GITHUB_REF_NAME set the way GitHub
    sets it for a `push: tags:` trigger."""
    run_script = _extract_workflow_run_step(RELEASE_WORKFLOW, "build", "tag")
    (tmp_path / "metadata").mkdir(parents=True, exist_ok=True)
    (tmp_path / "metadata" / "sdk_version.yaml").write_text(
        f"version: {declared}\nstatus:  released\n", encoding="utf-8"
    )
    github_output = tmp_path / "github_output.txt"
    github_output.write_text("", encoding="utf-8")
    return subprocess.run(
        ["bash", "-c", run_script],
        cwd=str(tmp_path),
        env={
            "PATH": __import__("os").environ["PATH"],
            "GITHUB_REF_NAME": tag,
            "GITHUB_OUTPUT": str(github_output),
        },
        capture_output=True,
        text=True,
        timeout=30,
    )


# ---------------------------------------------------------------------
# Case table: (tag, declared) -> should the step pass?
# ---------------------------------------------------------------------

_PASS_CASES = [
    ("v0.16.0", "0.16.0"),  # GA tag, bare declared, matching core.
    ("v0.16.0-rc1", "0.16.0-rc1"),  # rc tag, same-suffix declared.
    ("v0.16.0-rc2", "0.16.0"),  # rc tag, operator never re-bumped this rc (back-compat).
]

_FAIL_CASES = [
    ("v0.16.0", "0.15.0"),  # core mismatch.
    ("v0.16.0", "0.16.0-rc1"),  # GA tag, leftover rc suffix (the original #1902 direction).
    ("v0.16.0-rc2", "0.16.0-rc1"),  # rc tag, DIFFERENT rc suffix (this fix's new direction).
]


@pytestmark_bash
@pytest.mark.parametrize("tag,declared", _PASS_CASES)
def test_matching_tag_and_declared_pass(tmp_path, tag, declared):
    proc = _run_tag_step(tmp_path, tag, declared)
    assert proc.returncode == 0, proc.stdout + proc.stderr


@pytestmark_bash
@pytest.mark.parametrize("tag,declared", _FAIL_CASES)
def test_mismatched_tag_and_declared_fail(tmp_path, tag, declared):
    proc = _run_tag_step(tmp_path, tag, declared)
    assert proc.returncode != 0, (
        f"tag={tag!r} declared={declared!r} must fail: "
        f"stdout={proc.stdout!r} stderr={proc.stderr!r}"
    )
    # The step's `::error::` lines are plain `echo` (no `>&2`), same as
    # every other guard in this step -- they land on stdout, not stderr.
    assert "::error::" in proc.stdout


@pytestmark_bash
def test_rc2_tag_against_rc1_declared_is_the_1902_regression(tmp_path):
    """The exact reviewer-confirmed repro: a v0.16.0-rc2 build cut with a
    forgotten `version: 0.16.0-rc1` metadata bump must not silently
    self-report as rc1."""
    proc = _run_tag_step(tmp_path, "v0.16.0-rc2", "0.16.0-rc1")
    assert proc.returncode != 0
    assert "0.16.0-rc1" in proc.stdout
    assert "0.16.0-rc2" in proc.stdout
