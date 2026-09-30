# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Pin the cheap merge-queue citation gate added for #2206.

The normal metadata workflow checks citations on the PR merge ref, but it does
not run on ``merge_group`` and deliberately must not: that would put its full
validation sweep on every queue entry. This test keeps the queue-only workflow
small, gives it a stable requireable context, and ensures the citation checker
compares the speculative tree with the base SHA from the event payload.
"""

from __future__ import annotations

from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github" / "workflows" / "merge-queue-changelog-citations.yml"
METADATA_WORKFLOW = REPO / ".github" / "workflows" / "pr-metadata-validate.yml"


def _load(path: Path) -> dict:
    return yaml.safe_load(path.read_text(encoding="utf-8"))


def _on_block(doc: dict) -> dict:
    """Return ``on:`` despite PyYAML 1.1 resolving that key to ``True``."""
    return doc.get("on", doc.get(True)) or {}


def test_merge_queue_citation_workflow_is_dedicated_and_requireable() -> None:
    doc = _load(WORKFLOW)
    on_block = _on_block(doc)

    # `merge_group` is the trigger this gate exists for. `pull_request` and
    # `workflow_dispatch` are deliberate secondary legs (self-test +
    # manual smoke test, see the workflow's own header comment) -- but
    # nothing broader than these three may sneak in.
    assert "merge_group" in on_block
    assert set(on_block) == {"merge_group", "pull_request", "workflow_dispatch"}

    # The `pull_request` leg must stay path-filtered to the workflow's own
    # file, or ordinary PRs would pick up a second, differently-based
    # citation verdict alongside pr-metadata-validate.yml's.
    assert on_block["pull_request"]["paths"] == [
        ".github/workflows/merge-queue-changelog-citations.yml"
    ]

    # workflow_dispatch must carry a diff_base input, or a manual run can't
    # exercise the new-citation rule (see the workflow's header comment).
    assert "diff_base" in on_block["workflow_dispatch"]["inputs"]

    assert set(doc["jobs"]) == {"changelog-citations"}
    job = doc["jobs"]["changelog-citations"]
    assert job["name"] == "changelog citations · merge queue"
    assert "if" not in job

    # The expensive metadata sweep must remain off merge_group; this small
    # workflow is what covers the queue instead. Note the job name is stable
    # but NOT requireable as shaped: the pull_request leg is path-filtered, so
    # an ordinary PR reports no check run and a required context would stall at
    # "Expected -- Waiting for status to be reported". See the workflow's own
    # job comment.
    assert "merge_group" not in _on_block(_load(METADATA_WORKFLOW))


def test_merge_queue_citation_workflow_uses_the_event_base_sha() -> None:
    doc = _load(WORKFLOW)
    job = doc["jobs"]["changelog-citations"]
    assert len(job["steps"]) == 2

    # Least privilege, and one queue entry per ref. A mutation sweep showed
    # all three of these could be deleted without turning this file red, and
    # no other gate under scripts/ or tests/scripts/ enforces them.
    assert doc["permissions"] == {"contents": "read"}
    assert doc["concurrency"] == {
        "group": "${{ github.workflow }}-${{ github.ref }}",
        "cancel-in-progress": True,
    }

    checkout, check = job["steps"]
    # Pinned to a 40-char commit SHA, not a floating tag: the repo pins every
    # actions/checkout call-site that way.
    assert checkout["uses"].startswith("actions/checkout@")
    assert len(checkout["uses"].split("@", 1)[1].split()[0]) == 40
    assert checkout["with"]["fetch-depth"] == 0
    assert checkout["with"]["persist-credentials"] is False
    assert (
        check["env"]["DIFF_BASE"]
        == "${{ github.event.merge_group.base_sha || inputs.diff_base }}"
    )
    assert check["run"] == "python3 scripts/check_changelog_citations.py --verbose"
