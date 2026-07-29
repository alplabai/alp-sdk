"""Tests for scripts/dispatch-confirm.sh (issue #190).

dispatch-tan-parity.yml's "Confirm a run actually started" step used to be
inline shell whose only reachable failure was tan-cli's LIFETIME
repository_dispatch total_count being 0 -- a condition that stops being
reachable forever the first time the seam ever fires once (it now sits at
2). That made the step a gate that could only ever warn.

These tests stub `gh` on PATH and run the extracted script directly, so
both outcomes are PROVEN rather than read off the YAML:

  * a matching run appears during polling -> PASS.
  * no matching run appears, and tan-cli's most recent repository_dispatch
    run predates this push by more than the staleness threshold -> FAIL.
    This is the new reachable failure mode: it does not depend on
    lifetime==0, so it still fires even though tan-cli has dispatch runs
    in its history.
"""

import os
import stat
import subprocess
import sys
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "dispatch-confirm.sh"

FAKE_GH = """#!/usr/bin/env bash
# Stands in for `gh api URL [-q JQ_FILTER]`. Which canned JSON blob it
# answers with is picked by the query string, matching the two calls
# dispatch-confirm.sh actually makes; the -q filter (if any) is then
# applied with real jq so the script under test sees exactly what a real
# `gh api -q` would hand it.
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
    *per_page=5*) json="${FAKE_GH_POLL_JSON}" ;;
    *per_page=1*) json="${FAKE_GH_SUMMARY_JSON}" ;;
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


def _run(tmp_path: Path, *, poll_json: str, summary_json: str, dispatch_epoch: int,
          stale_threshold_s: int = 86400) -> subprocess.CompletedProcess:
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
def test_fails_when_no_run_appears_and_last_run_is_stale(tmp_path: Path) -> None:
    """No new run, and tan-cli's history has runs (lifetime > 0) but the most
    recent one is far older than the staleness threshold -> exit 1.

    This is the failure mode the old inline step could never produce once
    tan-cli's lifetime total_count left 0 -- proving the gate is alive
    again, not just re-worded.
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
    """No new run, but tan-cli's most recent dispatch run is recent -> warn,
    not fail (ordinary Actions queueing must not redden the push)."""
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
