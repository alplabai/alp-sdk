# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""A workflow producing a required context must trigger on `merge_group` (#1415).

GitHub raises `merge_group` -- not `push`, not `pull_request` -- for a
merge-queue entry. A required status check whose workflow does not carry that
trigger therefore never reports on the queue's speculative ref, and the entry
sits in `AWAITING_CHECKS` until the queue's `check_response_timeout_minutes`
expires and drops it. The queue does not fail loudly; it simply never merges
anything.

`dev`'s six required contexts come from three files:

    twister-shard 1/4 .. 4/4     .github/workflows/pr-twister.yml
    clang-format · diff-only     .github/workflows/pr-static-analysis.yml
    distro install · all         .github/workflows/pr-bootstrap-distro-install.yml

Read from branch protection on 2026-08-18:

    $ gh api repos/alplabai/alp-sdk/branches/dev/protection \\
        --jq '.required_status_checks.contexts'
    ["twister-shard 1/4","twister-shard 2/4","twister-shard 3/4",
     "twister-shard 4/4","clang-format · diff-only","distro install · all"]

That list lives in branch protection, not in this repo, so this gate cannot
derive it -- the three filenames below are pinned by hand and this docstring is
the record of where they came from. If the required-context set changes, this
list has to change with it; a stale entry here is caught by the
`the file exists` half of each case, a MISSING entry is not.
"""

from __future__ import annotations

from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOWS = REPO / ".github" / "workflows"

#: file -> the required context(s) it produces, for the failure message.
REQUIRED_CONTEXT_WORKFLOWS = {
    "pr-twister.yml": "twister-shard 1/4 .. 4/4",
    "pr-static-analysis.yml": "clang-format · diff-only",
    "pr-bootstrap-distro-install.yml": "distro install · all",
}


def _on_block(path: Path) -> dict:
    """The workflow's `on:` mapping.

    PyYAML resolves a bare `on:` key to the boolean `True` (YAML 1.1's
    y/n/on/off rule), which is why this looks it up under both spellings
    instead of `doc["on"]`.
    """
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    if "on" in doc:
        return doc["on"] or {}
    return doc.get(True) or {}


@pytest.mark.parametrize("filename,contexts", sorted(REQUIRED_CONTEXT_WORKFLOWS.items()))
def test_a_required_context_workflow_triggers_on_merge_group(filename, contexts):
    path = WORKFLOWS / filename
    assert path.is_file(), (
        f"{filename} is named here as the source of the required context(s) "
        f"{contexts!r} but does not exist. Either it was renamed -- update this "
        f"gate -- or the context is now produced by a different file."
    )
    triggers = _on_block(path)
    assert "merge_group" in triggers, (
        f"{filename} produces the required context(s) {contexts!r} but has no "
        f"`merge_group:` trigger, so it cannot report on a merge-queue entry "
        f"(#1415). Its triggers are: {sorted(triggers)}. A queued PR would wait "
        f"on that context until the queue's check_response_timeout_minutes "
        f"expires."
    )


@pytest.mark.parametrize("filename", sorted(REQUIRED_CONTEXT_WORKFLOWS))
def test_no_job_in_a_required_context_workflow_is_gated_to_pull_request(filename):
    """A `merge_group` trigger is inert if the job then skips itself.

    Adding the trigger and leaving `if: github.event_name == 'pull_request'` on
    the job produces a workflow that runs, reports nothing, and looks fixed.
    """
    doc = yaml.safe_load((WORKFLOWS / filename).read_text(encoding="utf-8"))
    offenders = [
        name
        for name, job in (doc.get("jobs") or {}).items()
        if "pull_request" in str(job.get("if", ""))
    ]
    assert offenders == [], (
        f"{filename}: job(s) {offenders} gate on `pull_request` in their "
        f"job-level `if:`, so they skip on a merge-queue entry and the "
        f"required context never reports (#1415)."
    )
