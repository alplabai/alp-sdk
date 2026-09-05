# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/refresh_issue_state_snapshot.py's idempotency
rule (#1950 round 2): `generated_at` must only move when the `issues`
object it records actually changed, so a no-op refresh leaves the file
byte-identical. Exercises `_pick_generated_at` directly -- the rest of the
script makes a live `gh` call and is out of scope for an offline unit test.
"""

import importlib.util
import json
import sys
import types
from pathlib import Path

import pytest

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


# -- refusing to wipe the snapshot on a `gh` auth/network failure (#1950 round 3) --


def test_all_unresolved_refuses_to_write():
    """MUST fail: every `_issue_state()` call returning None (a `gh` auth
    or network outage) must not be allowed to overwrite the snapshot with
    `{}` -- reproduces the round-2 review finding verbatim."""
    mod = _load()
    reason = mod._refuse_reason([494, 495, 496], {}, {})
    assert reason is not None
    assert "0 of 3" in reason


def test_all_unresolved_refuses_even_against_an_existing_snapshot():
    mod = _load()
    reason = mod._refuse_reason(
        [494, 495, 496], {}, {"494": "CLOSED", "495": "CLOSED", "496": "CLOSED"}
    )
    assert reason is not None


def test_drastic_shrink_against_disk_refuses_to_write():
    """9 issues still cited, `gh` only resolves 2 of them this run -- a
    partial outage, not 7 issues really vanishing. Must refuse (#1950
    round 4: on DROPPED KEYS, not a size ratio)."""
    mod = _load()
    existing = {str(n): "CLOSED" for n in range(1, 10)}
    numbers = list(range(1, 10))
    reason = mod._refuse_reason(numbers, {"1": "CLOSED", "2": "CLOSED"}, existing)
    assert reason is not None
    assert "did not resolve via `gh`" in reason


def test_dropping_under_half_still_refuses_the_old_blind_spot():
    """8 on disk, 4 silently dropped (exactly half) -- the old
    `len(issues) < len(existing_issues) / 2` ratio missed this (4 is not
    < 4) and let it degrade to a non-blocking warning. Dropped-key
    detection has no such threshold."""
    mod = _load()
    existing = {str(n): "CLOSED" for n in range(1, 9)}  # 1..8
    numbers = list(range(1, 9))
    issues = {str(n): "CLOSED" for n in range(1, 5)}  # 1..4 resolved, 5..8 dropped
    reason = mod._refuse_reason(numbers, issues, existing)
    assert reason is not None
    assert "4 still-cited issue(s)" in reason


def test_allow_shrink_overrides_the_dropped_key_refusal():
    """The explicit override (#1950 round 4): the same drop that refuses
    above must write clean when `allow_shrink=True`."""
    mod = _load()
    existing = {str(n): "CLOSED" for n in range(1, 10)}
    numbers = list(range(1, 10))
    reason = mod._refuse_reason(
        numbers, {"1": "CLOSED", "2": "CLOSED"}, existing, allow_shrink=True
    )
    assert reason is None


def test_legitimate_shrink_from_removed_citations_is_never_refused():
    """A citation that stopped being cited at all (removed from `numbers`,
    not just missing from `issues`) is not a drop -- refusing this
    unconditionally, with no override, was the round-3 bug (#1950 round
    4): a real cleanup that shrinks the cited set must write clean without
    needing --allow-shrink."""
    mod = _load()
    existing = {str(n): "CLOSED" for n in range(1, 10)}  # 9 were cited once
    numbers = [1, 2]  # only 2 are cited now; 3..9's citations were deleted
    reason = mod._refuse_reason(numbers, {"1": "CLOSED", "2": "CLOSED"}, existing)
    assert reason is None


def test_normal_refresh_does_not_refuse():
    """Non-vacuity: a real refresh that resolves everything it harvested,
    or a small legitimate shrink, must not trip the guard."""
    mod = _load()
    assert mod._refuse_reason([1, 2, 3], {"1": "OPEN", "2": "CLOSED", "3": "OPEN"}, {}) is None
    existing = {"1": "OPEN", "2": "CLOSED", "3": "OPEN"}
    assert mod._refuse_reason([1, 2], {"1": "OPEN", "2": "CLOSED"}, existing) is None


def test_no_citations_and_no_existing_snapshot_does_not_refuse():
    """A repo with zero driver-status citations at all (numbers == []) is
    not a `gh` failure -- must not refuse."""
    mod = _load()
    assert mod._refuse_reason([], {}, {}) is None


# -- _read_existing_issues -----------------------------------------------------


def test_read_existing_issues_missing_file(tmp_path):
    mod = _load()
    assert mod._read_existing_issues(tmp_path / "nope.json") == {}


def test_read_existing_issues_returns_the_issues_object(tmp_path):
    mod = _load()
    out = tmp_path / "issue-state-snapshot.json"
    out.write_text(
        json.dumps({"generated_at": "2020-01-01T00:00:00Z", "issues": {"1": "CLOSED"}}),
        encoding="utf-8",
    )
    assert mod._read_existing_issues(out) == {"1": "CLOSED"}


# -- main() wiring, not just the _refuse_reason predicate (#1950 round 4) -----


def test_main_refuses_and_leaves_snapshot_untouched_on_a_total_gh_outage(tmp_path, monkeypatch):
    """The tests above pin `_refuse_reason`'s PREDICATE, never its WIRING
    into `main()`. Deleting the `reason = _refuse_reason(...)` /
    `if reason: sys.exit(...)` call site restores the original defect -- a
    total `gh` outage silently overwrites the snapshot with `{}` and exits
    0 -- while every test above still passes unchanged. Drive `main()` for
    real with `_issue_state` stubbed to always return None (total outage)
    and check BOTH the exit code and that whatever was already on disk is
    byte-for-byte untouched."""
    mod = _load()
    out = tmp_path / "issue-state-snapshot.json"
    existing_bytes = json.dumps(
        {"generated_at": "2020-01-01T00:00:00Z", "issues": {"1": "CLOSED"}}
    ).encode("utf-8")
    out.write_bytes(existing_bytes)

    monkeypatch.setattr(mod, "find_citations", lambda root: [types.SimpleNamespace(issue=494)])
    monkeypatch.setattr(mod, "_issue_state", lambda n: None)
    monkeypatch.setattr(sys, "argv", ["refresh_issue_state_snapshot.py", "--out", str(out)])

    with pytest.raises(SystemExit) as exc_info:
        mod.main()

    assert exc_info.value.code != 0
    assert out.read_bytes() == existing_bytes
