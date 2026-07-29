"""Tests for scripts/dispatch-confirm.sh (issue #190).

dispatch-tan-parity.yml's "Confirm a run actually started" step used to be
inline shell whose only reachable failure was tan-cli's LIFETIME
repository_dispatch total_count being 0 -- a condition that stops being
reachable forever the first time the seam ever fires once (it now sits at
2). That made the step a gate that could only ever warn.

These tests stub `gh` on PATH and run the extracted script directly, so
outcomes are PROVEN rather than read off the YAML. Not every test below is
NEW coverage of the #190 change -- some pin behaviour that was already
correct and must stay that way. Sensitivity against the pre-#190 inline
script (reconstructed from git history) is noted per test:

  * test_passes_when_a_matching_run_appears -- KEPT-BEHAVIOR guard. Exits 0
    both before and after #190 (dispatch_epoch is only 5s in the past, well
    inside the old sliding `now - 300s` window too).
  * test_fails_when_lifetime_is_zero -- KEPT-BEHAVIOR guard. Exits 1 both
    before and after #190 (the one failure path the old step always had).
  * test_fails_when_no_run_appears_and_last_run_is_stale -- NEW coverage of
    #190's staleness path, exercised here via the bootstrap fallback (no
    PREV_DISPATCH_EPOCH baseline set): exits 0 pre-#190 (only warns, since
    lifetime=2 != 0) versus 1 now.
  * test_warns_when_no_run_appears_but_last_run_is_recent -- KEPT-BEHAVIOR
    guard for the bootstrap fallback path: exits 0 both before and after
    (recent last run stays under the threshold either way).
  * test_epoch_anchor_uses_dispatch_epoch_not_wall_clock -- NEW coverage.
    Proves the poll compares against the CALLER-supplied DISPATCH_EPOCH, not
    real wall-clock time. The pre-#190 script recomputed `now - 300s` on
    every iteration using jq's `now` (real time) -- a run 30 days old would
    never satisfy that, so this scenario would poll out and then fail/warn
    on the lifetime path instead of matching. It passes here specifically
    because the comparison is anchored to the (old, caller-supplied)
    DISPATCH_EPOCH rather than actual current time.
  * test_fails_when_self_calibrated_baseline_shows_no_run_since_previous_dispatch
    and test_warns_when_self_calibrated_baseline_shows_a_run_since_previous_dispatch
    -- NEW coverage of the self-calibrated staleness redesign (the BLOCKER
    fix to the #190 review: a flat 24h wall-clock constant was measurably
    false -- this workflow's own push trigger goes quiet for over 24h
    routinely and up to 110.8h in 60 days). PREV_DISPATCH_EPOCH did not
    exist pre-#190 at all, so both are new coverage with no pre-fix
    equivalent.
  * test_fails_when_gh_is_missing_from_path and
    test_fails_when_gh_probe_call_fails -- NEW coverage. The pre-#190 (and
    the first #190) script had no tooling/auth check at all: a broken `gh`
    burned the full poll window and then read identically to a genuine
    no-runs result.
  * test_workflow_invokes_the_script_and_captures_dispatch_epoch_first --
    NEW coverage of the workflow<->script SEAM itself. Every test above execs
    scripts/dispatch-confirm.sh directly, so reverting the workflow's confirm
    step to the old inline block would leave all of them green; this is the
    one that would catch that revert.
"""

from __future__ import annotations

import os
import shutil
import stat
import subprocess
import sys
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "dispatch-confirm.sh"
WORKFLOW = REPO / ".github" / "workflows" / "dispatch-tan-parity.yml"

FAKE_GH = """#!/usr/bin/env bash
# Stands in for `gh api URL [-q JQ_FILTER]`. Which canned JSON blob it
# answers with is picked by the query string, matching the calls
# dispatch-confirm.sh actually makes; the -q filter (if any) is then
# applied with real jq so the script under test sees exactly what a real
# `gh api -q` would hand it.
#
# The pre-poll probe call and the post-poll summary call hit the same
# `per_page=1` shape but differ on `event=repository_dispatch` (the probe
# has no event filter) -- FAKE_GH_PROBE_EXIT, if set, makes the probe call
# fail so tests can prove dispatch-confirm.sh treats that as a tooling
# failure, not a no-runs result.
shift  # drop 'api'
url="$1"; shift
jqfilter=""
while [ $# -gt 0 ]; do
    case "$1" in
        -q) jqfilter="$2"; shift 2 ;;
        *) shift ;;
    esac
done
case "$url" in
    *event=repository_dispatch*per_page=5*) json="${FAKE_GH_POLL_JSON}" ;;
    *event=repository_dispatch*per_page=1*) json="${FAKE_GH_SUMMARY_JSON}" ;;
    *per_page=1*)
        if [ -n "${FAKE_GH_PROBE_EXIT:-}" ]; then
            exit "${FAKE_GH_PROBE_EXIT}"
        fi
        json="{}"
        ;;
    *) json="{}" ;;
esac
if [ -n "${jqfilter}" ]; then
    echo "${json}" | jq -r "${jqfilter}"
else
    echo "${json}"
fi
"""


def _fake_bin(tmp_path: Path) -> Path:
    bindir = tmp_path / "bin"
    bindir.mkdir()
    gh = bindir / "gh"
    gh.write_text(FAKE_GH, encoding="utf-8")
    gh.chmod(gh.stat().st_mode | stat.S_IEXEC)
    return bindir


def _run(
    tmp_path: Path,
    *,
    poll_json: str,
    summary_json: str,
    dispatch_epoch: int,
    stale_threshold_s: int = 86400,
    prev_dispatch_epoch: int | None = None,
    probe_exit: str | None = None,
) -> subprocess.CompletedProcess:
    bindir = _fake_bin(tmp_path)
    env = dict(os.environ)
    env["PATH"] = f"{bindir}{os.pathsep}{env['PATH']}"
    env["GH_TOKEN"] = "fake-token"
    env["DISPATCH_EPOCH"] = str(dispatch_epoch)
    env["DISPATCH_CONFIRM_POLL_ATTEMPTS"] = "2"
    env["DISPATCH_CONFIRM_POLL_INTERVAL_S"] = "1"
    env["DISPATCH_CONFIRM_STALE_THRESHOLD_S"] = str(stale_threshold_s)
    env["FAKE_GH_POLL_JSON"] = poll_json
    env["FAKE_GH_SUMMARY_JSON"] = summary_json
    if prev_dispatch_epoch is not None:
        env["PREV_DISPATCH_EPOCH"] = str(prev_dispatch_epoch)
    else:
        env.pop("PREV_DISPATCH_EPOCH", None)
    if probe_exit is not None:
        env["FAKE_GH_PROBE_EXIT"] = probe_exit
    return subprocess.run(
        ["bash", str(SCRIPT)],
        env=env,
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )


@pytest.mark.skipif(sys.platform == "win32", reason="bash script, POSIX only")
def test_passes_when_a_matching_run_appears(tmp_path: Path) -> None:
    """A run created after DISPATCH_EPOCH shows up during polling -> exit 0."""
    now = int(time.time())
    dispatch_epoch = now - 5
    poll_json = (
        '{"workflow_runs":[{"created_at":"'
        + time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now))
        + '"}]}'
    )
    result = _run(tmp_path, poll_json=poll_json, summary_json="{}", dispatch_epoch=dispatch_epoch)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "::notice::tan-cli started 1 repository_dispatch run(s)" in result.stdout


@pytest.mark.skipif(sys.platform == "win32", reason="bash script, POSIX only")
def test_epoch_anchor_uses_dispatch_epoch_not_wall_clock(tmp_path: Path) -> None:
    """A run created shortly after a DISPATCH_EPOCH set 30 days in the past
    (not real 'now') still counts as a match.

    This is what anchoring on the caller-supplied DISPATCH_EPOCH buys over
    the pre-#190 inline version's sliding `now - 300s`, recomputed from real
    wall-clock time on every poll iteration: that comparison would need
    `created_at > (real_now - 300s)`, which a 30-day-old run can never
    satisfy, so the pre-fix script could not have matched this run at all.
    """
    old_epoch = int(time.time()) - 30 * 24 * 3600
    run_created = old_epoch + 15
    poll_json = (
        '{"workflow_runs":[{"created_at":"'
        + time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(run_created))
        + '"}]}'
    )
    result = _run(tmp_path, poll_json=poll_json, summary_json="{}", dispatch_epoch=old_epoch)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "::notice::tan-cli started 1 repository_dispatch run(s)" in result.stdout


@pytest.mark.skipif(sys.platform == "win32", reason="bash script, POSIX only")
def test_fails_when_no_run_appears_and_last_run_is_stale(tmp_path: Path) -> None:
    """No new run, no self-calibration baseline available, and tan-cli's
    history has runs (lifetime > 0) but the most recent one is far older
    than the bootstrap-fallback staleness threshold -> exit 1.

    This is the failure mode the pre-#190 inline step could never produce
    once tan-cli's lifetime total_count left 0.
    """
    now = int(time.time())
    summary_json = (
        '{"total_count":2,"workflow_runs":[{"created_at":"2020-01-01T00:00:00Z"}]}'
    )
    result = _run(
        tmp_path,
        poll_json='{"workflow_runs":[]}',
        summary_json=summary_json,
        dispatch_epoch=now,
        stale_threshold_s=86400,
    )
    assert result.returncode == 1, result.stdout + result.stderr
    assert "::error::tan-cli's most recent repository_dispatch run was at" in result.stdout
    assert "not a timing miss" in result.stdout


@pytest.mark.skipif(sys.platform == "win32", reason="bash script, POSIX only")
def test_warns_when_no_run_appears_but_last_run_is_recent(tmp_path: Path) -> None:
    """No new run, no self-calibration baseline, but tan-cli's most recent
    dispatch run is recent -> warn, not fail (ordinary Actions queueing must
    not redden the push)."""
    now = int(time.time())
    summary_json = (
        '{"total_count":2,"workflow_runs":[{"created_at":"'
        + time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now - 60))
        + '"}]}'
    )
    result = _run(
        tmp_path,
        poll_json='{"workflow_runs":[]}',
        summary_json=summary_json,
        dispatch_epoch=now,
        stale_threshold_s=86400,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "::warning::dispatched, but no NEW" in result.stdout


@pytest.mark.skipif(sys.platform == "win32", reason="bash script, POSIX only")
def test_fails_when_lifetime_is_zero(tmp_path: Path) -> None:
    """The original, still-reachable case: tan-cli has never had a
    repository_dispatch run at all."""
    now = int(time.time())
    result = _run(
        tmp_path,
        poll_json='{"workflow_runs":[]}',
        summary_json='{"total_count":0,"workflow_runs":[]}',
        dispatch_epoch=now,
    )
    assert result.returncode == 1, result.stdout + result.stderr
    assert "::error::tan-cli has NEVER had a repository_dispatch run" in result.stdout


@pytest.mark.skipif(sys.platform == "win32", reason="bash script, POSIX only")
def test_fails_when_self_calibrated_baseline_shows_no_run_since_previous_dispatch(
    tmp_path: Path,
) -> None:
    """No new run for THIS dispatch, and tan-cli's most recent run is no
    newer than alp-sdk's own PREVIOUS dispatch -> the seam produced nothing
    across an entire inter-dispatch period -> exit 1.

    This is the primary (non-fallback) staleness path the #190 review
    required: self-calibrated against our own dispatch history rather than a
    wall-clock constant, so it cannot false-positive on an ordinary quiet
    period the way a flat 24h threshold measurably did.
    """
    now = int(time.time())
    prev_dispatch_epoch = now - 100
    summary_json = (
        '{"total_count":2,"workflow_runs":[{"created_at":"'
        + time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now - 200))
        + '"}]}'
    )
    result = _run(
        tmp_path,
        poll_json='{"workflow_runs":[]}',
        summary_json=summary_json,
        dispatch_epoch=now,
        prev_dispatch_epoch=prev_dispatch_epoch,
    )
    assert result.returncode == 1, result.stdout + result.stderr
    assert "NO NEWER than alp-sdk's own previous dispatch" in result.stdout


@pytest.mark.skipif(sys.platform == "win32", reason="bash script, POSIX only")
def test_warns_when_self_calibrated_baseline_shows_a_run_since_previous_dispatch(
    tmp_path: Path,
) -> None:
    """No new run for THIS dispatch, but tan-cli's most recent run IS newer
    than alp-sdk's own previous dispatch -> the seam fired since last time,
    so this is ordinary queueing for THIS push specifically -> exit 0."""
    now = int(time.time())
    prev_dispatch_epoch = now - 300
    summary_json = (
        '{"total_count":2,"workflow_runs":[{"created_at":"'
        + time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now - 100))
        + '"}]}'
    )
    result = _run(
        tmp_path,
        poll_json='{"workflow_runs":[]}',
        summary_json=summary_json,
        dispatch_epoch=now,
        prev_dispatch_epoch=prev_dispatch_epoch,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "newer than alp-sdk's own previous dispatch" in result.stdout


@pytest.mark.skipif(sys.platform == "win32", reason="bash script, POSIX only")
def test_fails_when_gh_is_missing_from_path(tmp_path: Path) -> None:
    """No `gh` binary on PATH at all must FAIL outright, immediately -- not
    burn the whole poll window and land on the same message a genuine
    no-runs result gets."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    real_bash = shutil.which("bash")
    assert real_bash is not None, "need a real bash to exec the script under test"
    (bindir / "bash").symlink_to(real_bash)
    env = {"PATH": str(bindir), "DISPATCH_EPOCH": str(int(time.time()))}
    result = subprocess.run(
        ["bash", str(SCRIPT)],
        env=env,
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )
    assert result.returncode == 1, result.stdout + result.stderr
    assert "::error::gh CLI is not on PATH" in result.stdout


@pytest.mark.skipif(sys.platform == "win32", reason="bash script, POSIX only")
def test_fails_when_gh_probe_call_fails(tmp_path: Path) -> None:
    """`gh` is present but every call fails (expired/missing token, revoked
    App access) -> the pre-loop probe must FAIL fast, distinguishing THIS
    repo's tooling/auth problem from tan-cli genuinely having no runs."""
    now = int(time.time())
    result = _run(
        tmp_path,
        poll_json='{"workflow_runs":[]}',
        summary_json="{}",
        dispatch_epoch=now,
        probe_exit="1",
    )
    assert result.returncode == 1, result.stdout + result.stderr
    assert "::error::gh api probe call to tan-cli failed" in result.stdout


@pytest.mark.skipif(sys.platform == "win32", reason="bash script, POSIX only")
def test_workflow_invokes_the_script_and_captures_dispatch_epoch_first() -> None:
    """Guards the SEAM between the workflow and this script, not just the
    script in isolation. Every test above execs scripts/dispatch-confirm.sh
    directly, so reverting the workflow's confirm step back to the old
    inline shell block would leave all of them green -- this is the test
    that would catch that revert.
    """
    yaml = pytest.importorskip("yaml")
    doc = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    steps = doc["jobs"]["dispatch"]["steps"]

    dispatch_step = next(s for s in steps if s["name"] == "Dispatch planner-change to tan-cli")
    confirm_step = next(s for s in steps if s["name"] == "Confirm a run actually started")

    dispatch_run = dispatch_step["run"]
    epoch_pos = dispatch_run.find("DISPATCH_EPOCH=")
    call_pos = dispatch_run.find("gh api repos/alplabai/tan-cli/dispatches")
    assert epoch_pos != -1, "DISPATCH_EPOCH must be captured in the dispatch step"
    assert call_pos != -1, "the dispatch step must call the tan-cli /dispatches API"
    assert epoch_pos < call_pos, (
        "DISPATCH_EPOCH must be captured BEFORE the dispatch API call -- "
        "capturing it later (e.g. in the confirm step) leaves a 1-3s gap in "
        "which a fast, healthy dispatch's run is created before "
        "DISPATCH_EPOCH, and the confirm step's strict `>` comparison then "
        "misses it."
    )

    confirm_run = confirm_step["run"]
    assert "bash scripts/dispatch-confirm.sh" in confirm_run, (
        "the confirm step must invoke scripts/dispatch-confirm.sh -- every "
        "test in this file execs that script directly, so reverting this "
        "step to inline shell would silently drop all of that coverage "
        "while every test here kept passing."
    )
