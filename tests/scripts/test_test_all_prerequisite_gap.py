"""Regression test for alp-sdk#1396: scripts/test-all.sh must not report a
partial run (a REQUIRED stage skipped for a missing prerequisite) the same
way it reports a genuinely complete one.

This does NOT invoke scripts/test-all.sh's actual stages (no twister, no
cmake build, no pytest-in-pytest) -- it extracts the three pieces of the
real script that implement the SKIP/GAP bookkeeping (`run_stage()`,
`skip_stage()`, and the summary/exit-code block at the bottom of the file)
verbatim by marker, and drives them against synthetic fake stage functions
in a throwaway bash harness. Extracting by marker rather than duplicating
the logic by hand means a future edit to the real summary block either
keeps passing here (if it preserves the exit-code/tag contract this test
asserts) or breaks this test loudly (if the markers move/vanish) -- it
cannot silently drift out of sync with a hand-copied duplicate.

Covers:
  * a stage function returning 99 (the existing "prerequisite unavailable"
    convention) is tagged [GAP] in the summary and forces exit 2, not 0.
  * an explicit `skip_stage NAME REASON gap` call (clang-format missing,
    ZEPHYR_BASE unset, ...) does the same.
  * an explicit `skip_stage NAME REASON scope` call (--quick, --target dev)
    stays untagged and does NOT affect the exit code -- a deliberately
    scoped-out stage is not a gap.
  * a FAIL still wins (exit 1) even when a gap-skip is also present --
    failure reporting must not be diluted by the new gap bookkeeping.
  * `skip_stage` rejects a missing/invalid third argument loudly (exit 70)
    instead of silently defaulting -- every call site in the real script
    must say scope-or-gap on purpose.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TEST_ALL = REPO / "scripts" / "test-all.sh"

pytestmark = pytest.mark.skipif(
    not sys.platform.startswith("linux") or shutil.which("bash") is None,
    reason="drives extracted fragments of scripts/test-all.sh under bash; "
    "restricted to Linux for the same reason test_test_all_worktree.py is -- "
    "`shutil.which(\"bash\")` alone is NOT enough of a guard, because the "
    "windows-latest runner ships bash with Git for Windows, so the check "
    "passes and the POSIX fragments then fail on the Windows shell/quoting "
    "differences. test-all.sh is a Linux/WSL CI tool.",
)


def _extract_function(text: str, name: str) -> str:
    """Pull `name() { ... }` verbatim out of test-all.sh, from the
    declaration line to the first column-0 `}` -- matches how every
    stage/helper function in the file is written (no nested column-0
    closing brace inside a function body)."""
    pattern = rf"^{re.escape(name)}\(\) \{{\n.*?^\}}\n"
    m = re.search(pattern, text, re.MULTILINE | re.DOTALL)
    assert m, f"could not find `{name}() {{ ... }}` in {TEST_ALL} -- marker drifted"
    return m.group(0)


def _extract_summary_block(text: str) -> str:
    marker = "# -------- Summary "
    idx = text.index(marker)
    assert idx != -1, f"could not find the '{marker}' marker in {TEST_ALL}"
    return text[idx:]


@pytest.fixture(scope="module")
def harness_pieces():
    text = TEST_ALL.read_text()
    return {
        "run_stage": _extract_function(text, "run_stage"),
        "skip_stage": _extract_function(text, "skip_stage"),
        "summary": _extract_summary_block(text),
    }


def _run_harness(harness_pieces, body: str, tmp_path: Path) -> subprocess.CompletedProcess:
    """Assemble run_stage()+skip_stage()+the real summary block with a
    caller-supplied sequence of (fake) stage invocations in between, and
    execute it under bash. `body` runs with STAGE_* arrays already
    declared and TARGET/START/END already set."""
    script = tmp_path / "harness.sh"
    script.write_text(
        "#!/usr/bin/env bash\n"
        "set -uo pipefail\n"
        "declare -a STAGE_NAMES STAGE_STATUS STAGE_NOTES STAGE_KIND\n"
        "TARGET=dev\n"
        "START=0\n"
        f"{harness_pieces['run_stage']}\n"
        f"{harness_pieces['skip_stage']}\n"
        "fake_pass() { return 0; }\n"
        "fake_fail() { return 1; }\n"
        "fake_gap()  { return 99; }\n"
        f"{body}\n"
        "END=0\n"
        f"{harness_pieces['summary']}\n"
    )
    return subprocess.run(
        ["bash", str(script)],
        capture_output=True,
        text=True,
        timeout=30,
    )


def test_return_99_is_tagged_gap_and_forces_exit_2(harness_pieces, tmp_path):
    proc = _run_harness(
        harness_pieces,
        'run_stage "unit-pass" fake_pass\n'
        'run_stage "twister" fake_gap\n',
        tmp_path,
    )
    assert proc.returncode == 2, proc.stdout + proc.stderr
    assert "[GAP]" in proc.stdout
    assert "All runnable stages passed" not in proc.stdout
    assert "SKIPPED for a missing prerequisite" in proc.stdout


def test_explicit_gap_skip_stage_forces_exit_2(harness_pieces, tmp_path):
    proc = _run_harness(
        harness_pieces,
        'run_stage "unit-pass" fake_pass\n'
        'skip_stage "clang-format-diff" "clang-format not installed" gap\n',
        tmp_path,
    )
    assert proc.returncode == 2, proc.stdout + proc.stderr
    assert "[GAP]" in proc.stdout
    assert "All runnable stages passed" not in proc.stdout


def test_explicit_scope_skip_stage_stays_exit_0(harness_pieces, tmp_path):
    proc = _run_harness(
        harness_pieces,
        'run_stage "unit-pass" fake_pass\n'
        'skip_stage "doxygen" "--target dev (slow release-grade stage)" scope\n',
        tmp_path,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "[GAP]" not in proc.stdout
    assert "All runnable stages passed" in proc.stdout


def test_all_pass_no_skips_is_exit_0(harness_pieces, tmp_path):
    proc = _run_harness(
        harness_pieces,
        'run_stage "unit-pass" fake_pass\n',
        tmp_path,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "All runnable stages passed" in proc.stdout


def test_fail_wins_over_a_simultaneous_gap(harness_pieces, tmp_path):
    """A FAIL must still exit 1 -- gap bookkeeping is additive, it must
    never dilute or hide a real failure."""
    proc = _run_harness(
        harness_pieces,
        'run_stage "broken" fake_fail\n'
        'run_stage "twister" fake_gap\n',
        tmp_path,
    )
    assert proc.returncode == 1, proc.stdout + proc.stderr
    assert "stage(s) failed" in proc.stdout
    # The gap is still visible in the per-stage listing even though FAIL
    # took priority for the exit code:
    assert "[GAP]" in proc.stdout


def test_skip_stage_rejects_missing_kind_argument(harness_pieces, tmp_path):
    """Every real call site must say scope-or-gap on purpose -- an
    omitted/invalid third argument must fail loudly, not silently
    default to one or the other."""
    proc = _run_harness(
        harness_pieces,
        'skip_stage "some-stage" "some reason"\n',
        tmp_path,
    )
    assert proc.returncode == 70, proc.stdout + proc.stderr
    assert "internal error" in (proc.stdout + proc.stderr)


def test_skip_stage_rejects_invalid_kind_argument(harness_pieces, tmp_path):
    proc = _run_harness(
        harness_pieces,
        'skip_stage "some-stage" "some reason" bogus\n',
        tmp_path,
    )
    assert proc.returncode == 70, proc.stdout + proc.stderr
    assert "internal error" in (proc.stdout + proc.stderr)


def test_every_call_site_in_the_real_script_passes_a_kind():
    """Static sweep of the real file: every `skip_stage "name" "reason"`
    call must carry a third scope|gap argument. Guards against a future
    call site being added without deciding scope-vs-gap (which would
    otherwise only be caught at runtime, and only if that code path is
    actually exercised)."""
    text = TEST_ALL.read_text()
    calls = re.findall(r'skip_stage\s+"[^"]+"\s+"[^"]*"(\s+\S+)?', text)
    assert calls, "no skip_stage call sites found -- marker/regex drifted"
    for trailing in calls:
        kind = trailing.strip()
        assert kind in ("scope", "gap"), (
            f"skip_stage call site missing/invalid scope|gap argument: {trailing!r}"
        )


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
