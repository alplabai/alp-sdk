# SPDX-License-Identifier: Apache-2.0
"""
Tests for scripts/merge_queue_ejection_alarm.py (issue #1952).

Pins the actor-based ejection/dequeue distinction this script relies on --
validated against real timeline data pulled live from alp-sdk (see the
#1952 PR description for the raw events): PR #1887's three
`removed_from_merge_queue` events are all actor `github-merge-queue[bot]`
(the recorded silent-ejection incident); PRs #1673 and #1670
(`needs-silicon`, held open on purpose) were removed with actor `alpCaner`
-- a human dequeuing them, which must never alarm.
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import merge_queue_ejection_alarm as alarm  # noqa: E402


def _event(kind: str, created_at: str, login: str, event_id: int = 1) -> dict:
    return {
        "event": kind,
        "created_at": created_at,
        "actor": {"login": login},
        "id": event_id,
    }


# ---------------------------------------------------------------------
# last_queue_removal
# ---------------------------------------------------------------------


def test_bot_ejection_is_detected(monkeypatch):
    """PR #1887's real shape: enqueued, then the bot removed it and never
    re-added -- must be treated as a candidate ejection."""
    events = [
        _event("added_to_merge_queue", "2026-09-04T07:18:15Z", "alpCaner"),
        _event(
            "removed_from_merge_queue",
            "2026-09-04T09:44:43Z",
            "github-merge-queue[bot]",
            event_id=30545411102,
        ),
    ]
    monkeypatch.setattr(alarm, "_gh_json", lambda *a: events)

    removal = alarm.last_queue_removal(1887)

    assert removal is not None
    assert removal["id"] == 30545411102


def test_human_dequeue_is_not_an_ejection(monkeypatch):
    """PR #1673's real shape: enqueued, then a human (not the bot) removed
    it -- must NOT alarm, no matter how long it then sits green."""
    events = [
        _event("added_to_merge_queue", "2026-09-04T07:11:46Z", "alpCaner"),
        _event("removed_from_merge_queue", "2026-09-04T07:46:32Z", "alpCaner"),
    ]
    monkeypatch.setattr(alarm, "_gh_json", lambda *a: events)

    assert alarm.last_queue_removal(1673) is None


def test_never_enqueued_is_not_an_ejection(monkeypatch):
    """A PR with no queue events at all (never asked to merge, a draft,
    waiting on review, deliberately held) must not alarm."""
    monkeypatch.setattr(alarm, "_gh_json", lambda *a: [])

    assert alarm.last_queue_removal(1) is None


def test_currently_requeued_is_not_flagged(monkeypatch):
    """Removed once, then added again more recently -- the PR is (or should
    be) back in the queue; do not alarm on the stale removal."""
    events = [
        _event("added_to_merge_queue", "2026-09-04T07:18:15Z", "alpCaner"),
        _event(
            "removed_from_merge_queue",
            "2026-09-04T09:44:43Z",
            "github-merge-queue[bot]",
        ),
        _event("added_to_merge_queue", "2026-09-04T10:04:30Z", "alpCaner"),
    ]
    monkeypatch.setattr(alarm, "_gh_json", lambda *a: events)

    assert alarm.last_queue_removal(1887) is None


# ---------------------------------------------------------------------
# already_alarmed
# ---------------------------------------------------------------------


def test_already_alarmed_matches_marker(monkeypatch):
    marker = alarm.MARKER_TEMPLATE.format(event_id=42)
    monkeypatch.setattr(alarm, "_gh_json", lambda *a: [f"some comment\n\n{marker}"])

    assert alarm.already_alarmed(1887, 42) is True


def test_not_yet_alarmed_for_a_different_event_id(monkeypatch):
    marker = alarm.MARKER_TEMPLATE.format(event_id=42)
    monkeypatch.setattr(alarm, "_gh_json", lambda *a: [f"some comment\n\n{marker}"])

    # A second, later ejection on the same PR has a different event id and
    # must still alarm.
    assert alarm.already_alarmed(1887, 99) is False
