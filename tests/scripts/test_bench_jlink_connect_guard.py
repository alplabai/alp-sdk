# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#1318 -- a failed J-Link connect must never decode as empty output.

JLinkExe exits 0 even when it could not open the probe at all: every command
in the CommanderScript prints "Cannot connect to the probe/programmer." and
the run still ends "Script processing completed." Every AEN bench read-back
pipes that output through an ASCII decoder, so before the fix a total
infrastructure failure rendered as an EMPTY console block -- indistinguishable
from an app that ran and printed nothing, which is how it was misread on the
bench.

These tests pin the guard (`bench_jlink_assert_connected`) and, crucially,
that every J-Link read-back site actually calls it. The second half is the
part that rots: adding a new `... > /tmp/foo.out || true` read-back without
the assertion silently reintroduces the bug, and no other check would notice.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BENCH = REPO / "scripts" / "bench" / "aen"
ENV = BENCH / "bench-env.sh"


def _bash_can_run_a_script() -> bool:
    """True only when `bash` on PATH can actually execute something.

    Presence is not enough. On GitHub's windows-latest runner `bash`
    resolves to System32\\bash.exe -- the WSL launcher -- with no
    distribution installed. It exits 1 and prints a UTF-16 message about
    installing a distribution, so a `subprocess.run(["bash", ...])` returns
    1 for a reason that has nothing to do with the code under test.

    That is exactly how these tests reddened `python-smoke (windows-latest)`
    on every PR after alp-sdk#1318 landed: four failures whose assertion
    text (`assert 1 == 7`) looked like a real guard defect and was not.
    Probe by RUNNING something, never by `shutil.which`.
    """
    try:
        probe = subprocess.run(
            ["bash", "-c", "printf ok"],
            capture_output=True, text=True, timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return probe.returncode == 0 and probe.stdout.strip() == "ok"


# Evaluated once at collection. The guard itself is plain POSIX shell in
# bench-env.sh, so there is nothing to assert about it on a host with no
# working shell -- skip rather than fail, and say which host that is.
_NEEDS_BASH = pytest.mark.skipif(
    not _bash_can_run_a_script(),
    reason="no working `bash` on this host (Windows CI resolves the WSL "
           "launcher with no distribution installed); bench-env.sh is POSIX "
           "shell and cannot be exercised here",
)

# The verbatim JLinkExe transcript from the real bench failure (alp-sdk#1318),
# trimmed. Note it ends "Script processing completed." and JLinkExe exits 0 --
# that is exactly why the exit status could not be used.
REAL_FAILED_CONNECT = """\
J-Link Command File read successfully.
Processing script file...
J-Link>connect
J-Link connection not established yet but required for command.
Connecting to J-Link ...FAILED: Cannot connect to the probe/programmer.
J-Link>halt
J-Link connection not established yet but required for command.
Connecting to J-Link ...FAILED: Cannot connect to the probe/programmer.
J-Link>mem8 0x20000d00, 0x400
J-Link connection not established yet but required for command.
Connecting to J-Link ...FAILED: Cannot connect to the probe/programmer.
J-Link>qc

Script processing completed.
"""

# A successful read, as the same script produces when JLINK_SN is set.
REAL_GOOD_READ = """\
J-Link>mem8 0x20000d00, 0x400
20000D00 = 2A 2A 2A 20 42 6F 6F 74 69 6E 67 20 5A 65 70 68
20000D10 = 79 72 20 4F 53 20 62 75 69 6C 64 20 76 34 2E 34
J-Link>qc
Script processing completed.
"""


def _call_guard(out_file: Path) -> subprocess.CompletedProcess[str]:
    """Source bench-env.sh and invoke the guard on out_file.

    Deliberately passes NO absolute paths to bash. The drive-letter form is
    per-flavour (Git Bash /c/..., WSL /mnt/c/...) and `bash` on PATH is not
    necessarily the same one the developer uses interactively -- on this
    Windows host Python resolves System32\bash.exe (WSL) while the terminal
    is MSYS. Copying bench-env.sh next to the fixture and running with
    cwd=<dir> on bare filenames sidesteps translation on every platform.
    """
    workdir = out_file.parent
    (workdir / "bench-env.sh").write_bytes(ENV.read_bytes())
    script = (
        'source ./bench-env.sh; '
        f'bench_jlink_assert_connected "{out_file.name}" "unit-test"'
    )
    return subprocess.run(
        ["bash", "-c", script],
        cwd=workdir,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=60,
    )


@_NEEDS_BASH
def test_failed_connect_is_a_hard_error(tmp_path: Path) -> None:
    out = tmp_path / "jlink.out"
    out.write_text(REAL_FAILED_CONNECT, encoding="utf-8")

    res = _call_guard(out)

    assert res.returncode == 7, f"expected exit 7, got {res.returncode}\n{res.stderr}"
    # The operator must be told it was infrastructure, not a silent app.
    assert "could NOT connect" in res.stderr
    assert "not because the app was silent" in res.stderr
    # ...and be given the actionable next step, verbatim.
    assert "export JLINK_SN=" in res.stderr
    assert "0x4C013477" in res.stderr, "must name the AEN E8 SW-DP IDR to disambiguate the probes"


@_NEEDS_BASH
def test_successful_read_passes(tmp_path: Path) -> None:
    out = tmp_path / "jlink.out"
    out.write_text(REAL_GOOD_READ, encoding="utf-8")

    res = _call_guard(out)

    assert res.returncode == 0, f"guard must not fire on a good read:\n{res.stderr}"


@_NEEDS_BASH
def test_empty_output_is_a_hard_error(tmp_path: Path) -> None:
    """A missing/empty transcript is also a failure -- decoding it yields the
    same empty block, so it must not pass silently."""
    out = tmp_path / "jlink.out"
    out.write_text("", encoding="utf-8")

    res = _call_guard(out)

    assert res.returncode == 7
    assert "no J-Link output at all" in res.stderr


@_NEEDS_BASH
def test_missing_file_is_a_hard_error(tmp_path: Path) -> None:
    res = _call_guard(tmp_path / "does-not-exist.out")

    assert res.returncode == 7


# alp-sdk#1551: the guard enumerated FAILURE strings, so any failure mode that
# stopped JLinkExe before it printed one of them passed. Verbatim capture from
# the bench probe (J-Link Commander V9.46) running the pre-alp-sdk#1478 line
# shape, whose stray literal `n` made JLinkExe reject its own command line --
# this is the COMPLETE 147-byte transcript, nothing is trimmed.
REAL_REJECTED_COMMAND_LINE = """\
SEGGER J-Link Commander V9.46 (Compiled May 27 2026 12:24:58)
DLL version V9.46, compiled May 27 2026 12:23:54

Unknown command line option n.
"""


@_NEEDS_BASH
def test_transcript_with_no_commander_prompt_is_a_hard_error(tmp_path: Path) -> None:
    """JLinkExe that never ran the script must not read as a good connect.

    This transcript contains none of the guard's failure strings -- no "Cannot
    connect to the probe/programmer", no "Failed to connect to target" -- and it
    is not empty, so before alp-sdk#1551 it returned 0. On the read-back-only
    paths (reread.sh, the post-flash console dumps) this guard is the ONLY
    check, with no DPIDR gate behind it, so a 0 here decoded the absent output
    as a silent app.
    """
    out = tmp_path / "jlink.out"
    out.write_text(REAL_REJECTED_COMMAND_LINE, encoding="utf-8")

    res = _call_guard(out)

    assert res.returncode == 7, f"expected exit 7, got {res.returncode}\n{res.stderr}"
    # Say what actually happened -- an operator reading this must not go hunting
    # for a probe/cable fault when JLinkExe rejected its own arguments.
    assert "no 'J-Link>' command" in res.stderr
    assert "never executed" in res.stderr
    # The offending line has to be visible, not just described.
    assert "Unknown command line option n." in res.stderr


# Every `-CommanderScript ... > <file> || true` read-back site. Derived from the
# script bodies, NOT a hand-maintained allowlist -- a new read-back added
# without the assertion fails this test rather than slipping through.
_READBACK_RE = re.compile(
    r"^[ \t]*\S.*-CommanderScript\s.*?>\s*(?P<out>/tmp/\S+)\s*\|\|\s*true[ \t]*$",
    re.M,
)


def _bench_scripts() -> list[Path]:
    return sorted(BENCH.glob("*.sh"))


def test_every_jlink_readback_asserts_the_connection() -> None:
    """The guard is worthless if a read-back site forgets to call it.

    This is the check that keeps the fix from rotting: it derives the set of
    read-back sites from the actual `|| true` invocations in each script, so a
    newly added one must also add the assertion.
    """
    missing: list[str] = []

    for path in _bench_scripts():
        body = path.read_text(encoding="utf-8")
        for m in _READBACK_RE.finditer(body):
            out = m.group("out")
            # The assertion must reference the same capture file, within the
            # 6 lines that follow the invocation.
            tail = body[m.end() : m.end() + 600]
            if f"bench_jlink_assert_connected {out}" not in tail:
                line_no = body[: m.start()].count("\n") + 1
                missing.append(f"{path.name}:{line_no} reads {out} but never asserts the connect")

    assert not missing, "J-Link read-back with no connect assertion:\n  " + "\n  ".join(missing)


def test_the_readback_regex_actually_matches_something() -> None:
    """Guard against the guard: if the regex stops matching (a refactor changes
    the invocation shape), test_every_jlink_readback_asserts_the_connection
    would pass vacuously and cover nothing."""
    total = sum(len(_READBACK_RE.findall(p.read_text(encoding="utf-8"))) for p in _bench_scripts())
    assert total >= 6, f"expected >=6 J-Link read-back sites, matched {total} -- regex has drifted"


@pytest.mark.parametrize(
    "script",
    ["flash-jlink.sh", "flash-jlink-hp.sh", "flash-jlink-mramxip.sh", "flash-update-log-dual.sh"],
)
def test_every_jlink_mram_writer_has_the_dpidr_gate(script: str) -> None:
    """Separate concern from the connect guard: "did we reach a board" vs "is it
    the RIGHT board". Every script that writes MRAM over J-Link must confirm the
    AEN E8 SW-DP IDR first -- flash-update-log-dual.sh was the one that did not
    (alp-sdk#1318). Flashing the wrong board is the unrecoverable bench mistake.
    """
    body = (BENCH / script).read_text(encoding="utf-8")

    # The gate lives in bench_jlink_assert_aen_dpidr (bench-env.sh) so the
    # "which board" logic has ONE home -- it names the wrong board, rejects
    # both V2N probes, and hard-aborts. A caller satisfies this by invoking
    # the helper; an older inline grep pair also counts, but the helper is
    # what the callers now use.
    assert (
        "bench_jlink_assert_aen_dpidr" in body
        or ("AEN_DPIDR" in body and "GD32_DPIDR" in body and "ABORT" in body)
    ), f"{script} writes MRAM over J-Link with no SW-DP IDR gate"


# --- alp-sdk#1312: the "which board" gate, distinct from "any board" -------

_AEN_HIT  = "Found SW-DP with ID 0x4C013477"
_GD32_HIT = "Found SW-DP with ID 0x0BE12477"
_V2N_HIT  = "Found SW-DP with ID 0x6BA02477"


def _call_dpidr(out_file, text):
    """Source bench-env.sh and run the DPIDR gate on a transcript."""
    workdir = out_file.parent
    (workdir / "bench-env.sh").write_bytes(ENV.read_bytes())
    out_file.write_text(text, encoding="utf-8")
    script = (
        'source ./bench-env.sh; '
        f'bench_jlink_assert_aen_dpidr "{out_file.name}" "unit-test"'
    )
    return subprocess.run(
        ["bash", "-c", script], cwd=workdir, capture_output=True,
        text=True, encoding="utf-8", errors="replace", timeout=60,
    )


@_NEEDS_BASH
def test_aen_dpidr_accepted(tmp_path):
    res = _call_dpidr(tmp_path / "pf.out", _AEN_HIT + "\n")
    assert res.returncode == 0, res.stderr


@_NEEDS_BASH
def test_gd32_probe_is_refused_and_named(tmp_path):
    """The cloned-serial case. Landing a Flow C loadbin+go here would execute
    an AEN image on a different board under a different reservation."""
    res = _call_dpidr(tmp_path / "pf.out", _GD32_HIT + "\n")
    assert res.returncode == 4
    assert "GD32" in res.stderr
    assert "DIFFERENT board" in res.stderr, "must say it is not the board under this reservation"


@_NEEDS_BASH
def test_v2n_cm33_probe_is_refused_and_named(tmp_path):
    """Third probe on the bench, measured 2026-08-08: SW-DP 0x6BA02477,
    Cortex-M33 r0p4. Answers on SWD, not JTAG."""
    res = _call_dpidr(tmp_path / "pf.out", _V2N_HIT + "\n")
    assert res.returncode == 4
    assert "CM33" in res.stderr


@_NEEDS_BASH
def test_no_dpidr_at_all_is_refused(tmp_path):
    """A transcript with no DP ID must not pass -- absence of evidence is not
    evidence the right board answered."""
    res = _call_dpidr(tmp_path / "pf.out", "Connecting to J-Link ...O.K.\n")
    assert res.returncode == 4


def _loadbins(body: str) -> bool:
    """True when the script really issues `loadbin`, not merely mentions it.

    flash-run.sh's only occurrence is a comment reading "NO loadbin/setpc",
    so a bare substring test reports it as an ungated writer when it writes
    nothing at all.
    """
    return any(
        "loadbin" in line and not line.lstrip().startswith("#")
        for line in body.splitlines()
    )


def test_every_target_touching_helper_gates_on_the_dpidr():
    """ram-run.sh loadbins AND executes, so it needs the same gate the MRAM
    writers have. It did not have one (alp-sdk#1312) -- Flow C is the flow
    people run most often."""
    missing = [
        p.name for p in _bench_scripts()
        if (_loadbins(p.read_text(encoding="utf-8"))
            and "bench_jlink_assert_aen_dpidr" not in p.read_text(encoding="utf-8"))
    ]
    assert not missing, f"helper writes/executes on a target with no DPIDR gate: {missing}"


# --- alp-sdk#1488: `verifybin`'s outcome must gate the script, not just the
# connect check -------------------------------------------------------------
#
# flash-jlink.sh / flash-update-log-dual.sh / flash-update-log-firewall-probe.sh
# each issued `verifybin` and never read its result: the transcript went to a
# display-only pipe, the connect check was the only thing that could fail the
# script, so a `Verify failed.` line exited 0 and reported a good flash on a
# board that was NOT actually written. flash-jlink-hp.sh and
# flash-jlink-mramxip.sh had already been fixed for the identical defect under
# alp-sdk#1343 -- but nothing derived the FULL set of verifybin sites from the
# script bodies, so the other 3 went uncaught for months (before this test,
# this file had ZERO occurrences of "verifybin" or "verify successful"). This
# is that derivation: a NEW verifybin site that does not also grep ITS OWN
# capture file for both outcomes fails here rather than shipping ungated.

# Any `verifybin` invocation, wherever it lives (these all sit inside a
# `cat > /tmp/*.jlink <<EOF ... EOF` CommanderScript heredoc).
_VERIFYBIN_RE = re.compile(r"^[ \t]*verifybin[ \t]", re.M)

# The file each write step's JLinkExe transcript lands in, resolved from
# whichever capture shape follows the `-CommanderScript` invocation --
# either the SIGPIPE-prone `... | tee <file> | ...` shape (still used by
# flash-jlink-hp.sh / flash-jlink-mramxip.sh, deliberately left alone by
# alp-sdk#1488 finding 5 -- out of scope, pre-existing) or the
# write-then-grep-the-finished-file shape finding 5 moved the other three
# scripts to (`... > <file> 2>&1 || true`, then a separate grep pass).
_CAPTURE_RE = re.compile(r"\|\s*tee\s+(?P<tee>/tmp/\S+)|>\s*(?P<redir>/tmp/\S+)\s*2>&1")


def _verifybin_capture_file(body: str, after: int) -> str | None:
    """The transcript file the write step immediately after a `verifybin`
    line (at body[after:]) captures its JLinkExe output to. Bounded window --
    the capture always follows within the same CommanderScript write block,
    not somewhere else in the file."""
    m = _CAPTURE_RE.search(body[after : after + 2000])
    return (m.group("tee") or m.group("redir")) if m else None


def test_every_verifybin_site_is_gated_on_its_own_transcript() -> None:
    """Every `verifybin` site must grep its OWN transcript file.

    This half is pure text: it derives the set of verifybin sites from the
    actual `verifybin` invocations in each script, resolves each one's OWN
    transcript file, and pins that the greps name that file -- so a site
    accidentally checking a SIBLING script's stale transcript (the exact
    copy-paste trap the changelog calls out) fails here.

    It pins the FILENAME only, which is not the same as pinning the gate:
    deleting the `exit 3`s while leaving the greps in place still satisfies
    it. `test_every_verifybin_gate_actually_gates` below is the half that
    runs the gate and asserts it changes the exit status; the two are
    deliberately separate because only the second one needs a working bash
    (Windows CI has none, see _NEEDS_BASH) and this derivation must keep
    running there.
    """
    missing: list[str] = []

    for path in _bench_scripts():
        body = path.read_text(encoding="utf-8")
        for m in _VERIFYBIN_RE.finditer(body):
            line_no = body[: m.start()].count("\n") + 1
            out = _verifybin_capture_file(body, m.end())
            if out is None:
                missing.append(f"{path.name}:{line_no} issues verifybin but no capture file could be resolved")
                continue
            fail_re = re.compile(
                r'grep\s+-\w*\s+"verify failed\|verification failed\|mismatch"\s+' + re.escape(out)
            )
            ok_re = re.compile(r'grep\s+-\w*\s+"verify successful"\s+' + re.escape(out))
            if not fail_re.search(body):
                missing.append(f"{path.name}:{line_no} verifybin -> {out}, never greps that file for verify-failed/mismatch")
            if not ok_re.search(body):
                missing.append(f"{path.name}:{line_no} verifybin -> {out}, never greps that file for verify-successful")

    assert not missing, "verifybin site with no verify-outcome gate on its own transcript:\n  " + "\n  ".join(missing)


def test_the_verifybin_regex_actually_matches_something() -> None:
    """Guard against the guard: if the regex stops matching (a refactor
    changes the invocation shape), test_every_verifybin_site_is_gated_on_its_own_transcript
    would pass vacuously and cover nothing.

    Six SITES across five SCRIPTS (flash-jlink-mramxip.sh issues two, one per
    loadbin): flash-jlink.sh 1, flash-jlink-hp.sh 1, flash-jlink-mramxip.sh 2,
    flash-update-log-dual.sh 1, flash-update-log-firewall-probe.sh 1. Same
    floor as the sibling read-back guard above."""
    total = sum(len(_VERIFYBIN_RE.findall(p.read_text(encoding="utf-8"))) for p in _bench_scripts())
    assert total >= 6, f"expected >=6 verifybin sites, matched {total} -- regex has drifted"


# The shell block that turns a verifybin OUTCOME into an exit status. Two
# shapes exist in the tree and both open with the same explicit-failure `if`:
#
#   flash-jlink.sh / flash-jlink-hp.sh / flash-update-log-dual.sh /
#   flash-update-log-firewall-probe.sh   fail-`if`, then
#                                        `if ! grep -qi "verify successful"`
#   flash-jlink-mramxip.sh               fail-`if`, then a `grep -ci` COUNT
#                                        compared against its two passes
_VERIFY_GATE_START_RE = re.compile(
    r'^[ \t]*if\s+grep\s+-\w*\s+"verify failed\|verification failed\|mismatch"'
    r"\s+(?P<out>/tmp/\S+)\s*;\s*then[ \t]*$"
)


def _verify_gate_block(body: str) -> tuple[str, str] | None:
    """`(capture-file, shell block)` for a script's verify gate, or None.

    The block is the contiguous source region from the explicit-failure `if`
    through the `fi` that closes the success check -- everything that turns a
    transcript into an exit status and nothing else, so it can be run
    standalone against a synthetic transcript.
    """
    lines = body.splitlines()
    start: int | None = None
    out = ""
    for i, line in enumerate(lines):
        m = _VERIFY_GATE_START_RE.match(line)
        if m:
            start, out = i, m.group("out")
            break
    if start is None:
        return None

    depth = 0
    saw_success = False
    for i in range(start, len(lines)):
        stripped = lines[i].strip()
        if re.match(r"^if\b", stripped):
            depth += 1
        if "verify successful" in stripped.lower():
            saw_success = True
        if stripped == "fi":
            depth -= 1
            if depth == 0 and saw_success:
                return out, "\n".join(lines[start : i + 1]) + "\n"
    return None


def _run_verify_gate(
    tmp_path: Path, block: str, out: str, transcript: str | None
) -> subprocess.CompletedProcess[str]:
    """Run one extracted verify gate against a synthetic JLinkExe transcript.

    `transcript=None` means the file does not exist at all. Same
    no-absolute-paths discipline as _call_guard: the gate's `/tmp/...` path is
    rewritten to a bare filename and bash runs with cwd=tmp_path, so the
    drive-letter flavour of whichever bash Python resolves cannot matter.
    `set -e` matches the real scripts, all of which run under errexit.

    The block is written to a FILE and run as `bash gate.sh`, never handed to
    `bash -c` as a string. On Windows, `subprocess` rebuilds the argument list
    into one command line (`list2cmdline`) which the MSYS runtime then re-parses,
    and double quotes inside a `$( ... )` command substitution do not survive the
    round trip: `v=$(grep -ci "verify successful" t.out || true)` reaches grep as
    the three arguments `-ci`, `"verify`, `successful"`, so grep reports
    `grep: successful": No such file or directory` and the count comes back empty.
    flash-jlink-mramxip.sh's gate is the only one that puts a quoted grep inside a
    command substitution -- the other four use a bare `if grep -qi "..."`, which
    survives -- so this manifested as exactly one parametrisation failing, naming a
    script whose gate is CORRECT (run from a file it returns 0 on a good transcript
    and 3 on a failing one). GitHub's windows-latest never saw it because
    _NEEDS_BASH skips there; a developer with Git Bash installed sees a red test
    pointing at the wrong file, which is the same misleading-failure class
    _NEEDS_BASH exists to prevent. A file has no second parse, so it cannot recur.
    """
    name = "transcript.out"
    target = tmp_path / name
    if transcript is None:
        target.unlink(missing_ok=True)
    else:
        target.write_text(transcript, encoding="utf-8")
    gate = tmp_path / "gate.sh"
    # write_bytes, not write_text: the gate must keep LF endings whatever the
    # host default is -- CRLF inside the block would reach bash as stray \r.
    gate.write_bytes(("set -e\n" + block.replace(out, name)).encode("utf-8"))
    return subprocess.run(
        ["bash", gate.name],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=60,
    )


def _scripts_with_verifybin() -> list[str]:
    """Derived, never hand-maintained -- a new verifybin script is covered the
    moment it lands, which is the whole point of this file."""
    return [p.name for p in _bench_scripts() if _VERIFYBIN_RE.search(p.read_text(encoding="utf-8"))]


@_NEEDS_BASH
@pytest.mark.parametrize("script", _scripts_with_verifybin())
def test_every_verifybin_gate_actually_gates(script: str, tmp_path: Path) -> None:
    """The gate must CHANGE THE EXIT STATUS, not merely mention the strings.

    test_every_verifybin_site_is_gated_on_its_own_transcript is text-only and
    is fail-open on the alp-sdk#1488 defect itself: delete both `exit 3` from
    a gate, or invert `if ! grep -qi "verify successful"` to `if grep -qi
    ...`, and the greps still sit in the body against the right file, so it
    stays green while a failed flash reports success again. This one extracts
    the gate and RUNS it, so those mutations go red:

      - a transcript whose verify FAILED must exit non-zero (3, the status
        flash-all-flowd.sh maps to the FLASH-UNVERIFIED batch-summary entry);
      - a transcript whose verifies all SUCCEEDED must exit 0;
      - those two statuses must DIFFER (an inverted polarity fails both, so
        equality alone catches it);
      - `Verify failed.` alongside a full set of success lines must still be
        non-zero, so the explicit-failure branch cannot be deleted and hidden
        behind the success check;
      - a missing or empty transcript must be non-zero -- absence of a
        `Verify successful.` line is not evidence the verify passed.
    """
    body = (BENCH / script).read_text(encoding="utf-8")
    sites = len(_VERIFYBIN_RE.findall(body))
    found = _verify_gate_block(body)
    assert found is not None, f"{script} issues verifybin but has no runnable verify-outcome gate"
    out, block = found

    # One "Verify successful." per verifybin issued: flash-jlink-mramxip.sh
    # writes two blobs and its gate demands both passes, so a single success
    # line is a FAILURE there, not a pass.
    ok_lines = "Verify successful.\n" * sites
    header = "J-Link>verifybin\n"

    good = _run_verify_gate(tmp_path, block, out, header + ok_lines)
    bad = _run_verify_gate(tmp_path, block, out, header + "Verify failed.\n")
    bad_with_ok = _run_verify_gate(tmp_path, block, out, header + "Verify failed.\n" + ok_lines)
    empty = _run_verify_gate(tmp_path, block, out, "")
    absent = _run_verify_gate(tmp_path, block, out, None)

    assert good.returncode == 0, (
        f"{script}: a fully successful verify must pass the gate, got "
        f"{good.returncode}\n{good.stdout}{good.stderr}"
    )
    assert bad.returncode != 0, (
        f"{script}: 'Verify failed.' must fail the gate -- it exited "
        f"{bad.returncode}, the exact alp-sdk#1488 defect\n{bad.stdout}{bad.stderr}"
    )
    assert bad.returncode == 3, f"{script}: expected exit 3, got {bad.returncode}"
    assert bad.returncode != good.returncode, (
        f"{script}: the gate returns {bad.returncode} for BOTH a failed and a "
        "successful verify -- it does not gate"
    )
    assert bad_with_ok.returncode == 3, (
        f"{script}: 'Verify failed.' alongside {sites} success line(s) must still "
        f"fail, got {bad_with_ok.returncode}"
    )
    assert empty.returncode == 3, f"{script}: an empty transcript must fail, got {empty.returncode}"
    assert absent.returncode == 3, f"{script}: a missing transcript must fail, got {absent.returncode}"
