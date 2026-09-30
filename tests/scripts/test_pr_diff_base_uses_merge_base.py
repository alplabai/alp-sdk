# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""The clang-format diff base must be a merge-base, not the raw PR base sha (#2349).

`pr-static-analysis.yml`'s "Determine diff base" step feeds `git diff` a
single ref that the `clang-format-diff` step then diffs against `HEAD`. If
that ref is `github.event.pull_request.base.sha` itself (a two-dot diff),
the result disagrees with what a reviewer sees whenever the branch head is a
merge commit -- e.g. a long-lived feature branch that merged `dev` into
itself. A merge commit can reshape content `dev` already changed since the
branch point, so a two-dot diff includes lines that are not part of this
PR's own changes. `git merge-base base head` (matching GitHub's own
three-dot PR-diff semantics) is the fix; this test pins that the "pull_request"
branch of the step actually calls `git merge-base` rather than echoing
`$BASE_SHA` straight through.
"""

from __future__ import annotations

from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github" / "workflows" / "pr-static-analysis.yml"


def _determine_diff_base_run() -> str:
    doc = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    job = doc["jobs"]["clang-format-diff"]
    for step in job["steps"]:
        if step.get("name") == "Determine diff base":
            return step["run"]
    raise AssertionError("pr-static-analysis.yml: no 'Determine diff base' step found")


def test_diff_base_step_uses_merge_base_not_raw_base_sha():
    run = _determine_diff_base_run()
    assert "git merge-base" in run, (
        "'Determine diff base' no longer computes a merge-base (#2349): a plain "
        "two-dot diff against the PR's base sha disagrees with the rendered PR "
        "diff whenever the branch head is a merge commit."
    )
    assert 'echo "ref=$BASE_SHA"' not in run, (
        "'Determine diff base' still echoes the raw $BASE_SHA as the diff ref "
        "(#2349) -- it must resolve through `git merge-base` first."
    )
