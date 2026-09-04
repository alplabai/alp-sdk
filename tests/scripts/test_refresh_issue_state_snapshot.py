# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/refresh_issue_state_snapshot.py's idempotency
rule (#1950 round 2): `generated_at` must only move when the `issues`
object it records actually changed, so a no-op refresh leaves the file
byte-identical. Exercises `_pick_generated_at` directly -- the rest of the
script makes a live `gh` call and is out of scope for an offline unit test.
"""

import importlib.util
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "refresh_issue_state_snapshot.py"


def _load():
    spec = importlib.util.spec_from_file_location("refresh_issue_state_snapshot", SCRIPT)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_unchanged_issues_keeps_existing_timestamp(tmp_path):
    """A no-op refresh (same issues, called again later) must not move
    generated_at -- that is the whole point of the fix."""
    mod = _load()
    out = tmp_path / "issue-state-snapshot.json"
    out.write_text(
        json.dumps({"generated_at": "2020-01-01T00:00:00Z", "issues": {"1": "CLOSED"}}),
        encoding="utf-8",
    )
    assert mod._pick_generated_at(out, {"1": "CLOSED"}) == "2020-01-01T00:00:00Z"


def test_changed_issues_gets_a_fresh_timestamp(tmp_path):
    mod = _load()
    out = tmp_path / "issue-state-snapshot.json"
    out.write_text(
        json.dumps({"generated_at": "2020-01-01T00:00:00Z", "issues": {"1": "CLOSED"}}),
        encoding="utf-8",
    )
    fresh = mod._pick_generated_at(out, {"1": "CLOSED", "2": "OPEN"})
    assert fresh != "2020-01-01T00:00:00Z"


def test_missing_file_gets_a_fresh_timestamp(tmp_path):
    mod = _load()
    out = tmp_path / "issue-state-snapshot.json"
    assert not out.exists()
    ts = mod._pick_generated_at(out, {"1": "CLOSED"})
    assert ts and ts != "2020-01-01T00:00:00Z"
