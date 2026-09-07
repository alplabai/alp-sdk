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

import os
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


@_NEEDS_BASH
def test_aen_dpidr_is_not_environment_overridable(tmp_path):
    """alp-sdk#1716: a pre-set AEN_DPIDR must NOT win.

    Before the fix, bench-env.sh assigned
    `AEN_DPIDR="${AEN_DPIDR:-4C013477}"` -- a caller-exported value silently
    replaced the real AEN E8 constant, so `export AEN_DPIDR=<whatever the
    wrong board answers>` before running any of 5 of the 6
    bench_jlink_assert_aen_dpidr call sites made the wrong-board interlock
    accept that board. Simulate exactly that: set AEN_DPIDR to a bogus value
    *before* sourcing bench-env.sh (the same shape an operator's `export
    AEN_DPIDR=...` would leave), and feed the guard a transcript that answers
    with that bogus value, not the real AEN E8 ID. A correctly-hardened
    bench-env.sh ignores the caller's AEN_DPIDR and still expects the real
    4C013477, so this must be REFUSED (returncode 4), not silently accepted
    (returncode 0).

    Deliberately does NOT use `bash -c "...$AEN_DPIDR..."` or
    `subprocess.run(..., env=...)`: on this host, a literal `$VAR` inside a
    `-c` argument does not survive the Windows argv round-trip (the exact
    `list2cmdline`/MSYS re-parse trap `_run_verify_gate` above documents),
    and an env var added only via the `env=` kwarg was observed NOT to reach
    this MSYS bash at all -- both would make this test pass vacuously
    regardless of the fix. Writing the override as a literal (no `$`)
    assignment in a script FILE run as `bash gate.sh` sidesteps both: proven
    by hand to flip 0 (pre-fix) / 4 (post-fix) for this exact scenario.
    """
    workdir = tmp_path
    out = workdir / "pf.out"
    out.write_text("Found SW-DP with ID 0xDEADBEEF\n", encoding="utf-8")
    (workdir / "bench-env.sh").write_bytes(ENV.read_bytes())
    gate = workdir / "gate.sh"
    gate.write_bytes(
        (
            'AEN_DPIDR="DEADBEEF"\n'          # the attempted override
            "source ./bench-env.sh\n"
            f'bench_jlink_assert_aen_dpidr "{out.name}" "unit-test"\n'
        ).encode("utf-8")
    )
    res = subprocess.run(
        ["bash", "gate.sh"], cwd=workdir, capture_output=True,
        text=True, encoding="utf-8", errors="replace", timeout=60,
    )
    assert res.returncode == 4, (
        "AEN_DPIDR override was honoured -- the wrong-board MRAM-write "
        f"interlock is bypassable via a bare env var (alp-sdk#1716)\n{res.stderr}"
    )


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


# --- alp-sdk#2025 follow-up: bench_atoc_replace_guard, shared by every script
# that commits a fresh ATOC (flash-run.sh, flash-run-dualcore.sh,
# flash-update-log-dual.sh, flash-update-log-firewall-probe.sh) ------------

# A real multi-entry ATOC table, trimmed from the alp-sdk#2025 incident
# report (PR #2026): two DEVICE rows plus the A32 Linux boot chain that a
# blind ALP-HE-only write silently delisted.
_REAL_MULTI_ENTRY_ATOC = """\
|   DEVICE |  CM0+  | 0x8057C6F0 | 0x8057BCF0 | ---------- | ---------- |      312 |  0.5.0| u V  |
|   DEVICE |  CM0+  | 0x805C1EC0 | 0x805C14C0 | ---------- | ---------- |      372 |  0.5.0| u V  |
| BOOTLOAD | A32_0  | 0x80002000 | 0x8057A8F0 | ---------- | 0x80002000 |    28813 |  0.4.3| u VB |
|  A32_APP | A32_0  | 0x80020000 | 0x8057B2F0 | ---------- | ---------- |  2290048 |  1.0.0| u V  |
|   HP_APP | M55-HP | 0x8057D230 | 0x8057C830 | 0x50000000 | 0x50000000 |     4480 |  1.0.0| uLVB |
|   HE_APP | M55-HE | 0x8057EDB0 | 0x8057E3B0 | 0x58000000 | 0x58000000 |     4480 |  1.0.0| uLVB |
"""

# The same board immediately after a compliant write: only DEVICE and the
# entries the caller itself is about to (re)write survive.
_ONLY_ALLOWED_ATOC = """\
|   DEVICE |  CM0+  | 0x8057C6F0 | 0x8057BCF0 | ---------- | ---------- |      312 |  0.5.0| u V  |
|   ALP-HE | M55-HE | 0x8057EDB0 | 0x8057E3B0 | 0x58000000 | 0x58000000 |     4480 |  1.0.0| uLVB |
"""

_NO_ATOC = "No ATOC found on target device.\n"


_COMPLIANT_BANNER = "SES A1 v1.8.0 Feb 20 2026\n"

# Real captures off `e1m-aen-evk-01`, 2026-09-07, ANSI intact -- verbatim,
# not synthesised. alp-sdk#2026 round-2 review measured these against the
# fixed guard by hand before this suite pinned them:
#
# `getbanner`: SETOOLS pads the banner text with a LEADING SPACE
# (" SES A1 v1.110.0 ..."), which a bare `^SES` anchor rejects outright --
# every real run aborted before this fix, training the operator to always
# pass --replace-atoc.
_REAL_GETBANNER = (
    "[INFO] port override /dev/ttyUSB0\n"
    "[INFO] /dev/ttyUSB0 open Serial port success \n"
    "[INFO] baud rate 57600\n"
    "[INFO] Connecting to target...Device connected\n"
    "\x1b[94m SES A1 v1.110.0 Mar  4 2026 19:06:23 \x1b[0m\n"
    " \n"
    "\x1b[?25h\n"
    "\x1b[0m\n"
)

# `gettoc`, 9 rows: the actual A32 Linux boot chain (BOOTLOAD/A32_APP/
# HP_APP/HE_APP) alongside the two baseline SE firmware banks (SERAM0/
# SERAM1). SETOOLS colours the WHOLE LINE, not just the cell text (the
# closing `\x1b[0m` opens the NEXT line, not the same one) -- a table-row
# match anchored to a bare leading `|` never matches a real row at all,
# silently computing an EMPTY resident set on every real coloured capture.
_REAL_GETTOC_9ROW = (
    "[INFO] port override /dev/ttyUSB0\n"
    "[INFO] /dev/ttyUSB0 open Serial port success \n"
    "[INFO] baud rate 57600\n"
    "[INFO] Connecting to target...Device connected\n"
    "\x1b[94m +----------+--------+------------+------------+------------+------------+----------+-----------+--------+----------+\n"
    "\x1b[0m\x1b[94m |   Name   |  CPU   | Store Addr |  Obj Addr  | Dest Addr  | Boot Addr  |   Size   |  Version  |  Flags | Time (ms)|\n"
    "\x1b[0m\x1b[94m +----------+--------+------------+------------+------------+------------+----------+-----------+--------+----------+\n"
    "\x1b[0m\x1b[94m |    DEVICE|   CM0+ | 0x8057c6f0 | 0x8057BCF0 | ---------- | ---------- |      312 |      0.5.0|u V     |    14.92 |\n"
    "\x1b[0m\x1b[94m |    DEVICE|   CM0+ | 0x805c1ec0 | 0x805C14C0 | ---------- | ---------- |      372 |      0.5.0|u V     |    15.04 |\n"
    "\x1b[0m\x1b[94m |  * SERAM0|   CM0+ | ---------- | 0x000000C0 | ---------- | ---------- |    64508 |    1.110.0|u s     |     0.00 |\n"
    "\x1b[0m\x1b[94m |    SERAM1|   CM0+ | ---------- | 0x00020AC0 | ---------- | ---------- |    64508 |    1.110.0|------- |     0.00 |\n"
    "\x1b[0m\x1b[94m |  BOOTLOAD| A32_0  | 0x80002000 | 0x8057A8F0 | ---------- | 0x80002000 |    28816 |      0.4.3|u VB    |    16.89 |\n"
    "\x1b[0m\x1b[94m |   A32_APP| A32_0  | 0x80020000 | 0x8057B2F0 | ---------- | ---------- |  2290048 |      1.0.0|u V     |   189.01 |\n"
    "\x1b[0m\x1b[94m |    HP_APP| M55-HP | 0x8057d230 | 0x8057C830 | 0x50000000 | 0x50000000 |     4480 |      1.0.0|uLVB    |    15.90 |\n"
    "\x1b[0m\x1b[94m |    HE_APP| M55-HE | 0x8057edb0 | 0x8057E3B0 | 0x58000000 | 0x58000000 |     4480 |      1.0.0|uLVB    |    15.55 |\n"
    "\x1b[0m\x1b[94m +----------+--------+------------+------------+------------+------------+----------+-----------+--------+----------+\n"
    "\x1b[0m\x1b[?25h\n"
    "\x1b[0m\n"
)

# `gettoc`, the same board's CLEAN, compliant state: only DEVICE and its own
# ALP-HE app entry, plus the two baseline SERAM0/SERAM1 banks. Must pass
# (rc=0) with allowed=["ALP-HE"] -- SERAM0/SERAM1 are SE firmware, never
# touched by app-write-mram -p (only a System Package update rewrites them,
# docs/aen-se-services.md section 0.1), so they must never count as a
# would-be-delisted "extra" entry. Without that exemption the guard refused
# this exact real, compliant capture unconditionally.
_REAL_GETTOC_CLEAN = (
    "[INFO] port override /dev/ttyUSB0\n"
    "[INFO] /dev/ttyUSB0 open Serial port success \n"
    "[INFO] baud rate 57600\n"
    "[INFO] Connecting to target...Device connected\n"
    "\x1b[94m +----------+--------+------------+------------+------------+------------+----------+-----------+--------+----------+\n"
    "\x1b[0m\x1b[94m |   Name   |  CPU   | Store Addr |  Obj Addr  | Dest Addr  | Boot Addr  |   Size   |  Version  |  Flags | Time (ms)|\n"
    "\x1b[0m\x1b[94m +----------+--------+------------+------------+------------+------------+----------+-----------+--------+----------+\n"
    "\x1b[0m\x1b[94m |    DEVICE|   CM0+ | 0x80564530 | 0x80563B30 | ---------- | ---------- |      312 |      0.5.0|u V     |    14.92 |\n"
    "\x1b[0m\x1b[94m |    DEVICE|   CM0+ | 0x805c1ec0 | 0x805C14C0 | ---------- | ---------- |      372 |      0.5.0|u V     |    15.04 |\n"
    "\x1b[0m\x1b[94m |  * SERAM0|   CM0+ | ---------- | 0x000000C0 | ---------- | ---------- |    64508 |    1.110.0|u s     |     0.00 |\n"
    "\x1b[0m\x1b[94m |    SERAM1|   CM0+ | ---------- | 0x00020AC0 | ---------- | ---------- |    64508 |    1.110.0|------- |     0.00 |\n"
    "\x1b[0m\x1b[94m |    ALP-HE| M55-HE | 0x80565070 | 0x80564670 | 0x58000000 | 0x58000000 |   110356 |      1.0.0|uLVB    |    29.02 |\n"
    "\x1b[0m\x1b[94m +----------+--------+------------+------------+------------+------------+----------+-----------+--------+----------+\n"
    "\x1b[0m\x1b[?25h\n"
    "\x1b[0m\n"
)


def _call_atoc_guard(
    tmp_path: Path,
    replace_atoc: str,
    allowed: list[str],
    se_uart: str | None,
    gettoc_output: str | None,
    setools_has_maintenance: bool = True,
    gettoc_exit: int = 0,
    banner_output: str | None = None,
    banner_exit: int = 0,
    tag: str = "test-tag",
    tmpdir: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run bench_atoc_replace_guard (bench-env.sh) against a synthetic
    `maintenance -opt gettoc` (and -opt getbanner) transcript, without
    touching any real bench, SETOOLS install, or probe.

    The stub `maintenance` distinguishes `-opt getbanner` from `-opt gettoc`
    by scanning argv for the `-opt` value -- the guard now reads a banner
    first (docs/debugging-aen.md:548's "SES <rev> v<version>") to confirm the
    SE-UART actually answered, before trusting a gettoc read from it.
    `banner_output` defaults to a compliant banner so callers that don't care
    about the banner path keep exercising only what they name.

    Same file-based-script discipline as `_call_dpidr` /
    `test_aen_dpidr_is_not_environment_overridable` above: literal
    assignments in a script FILE run as `bash gate.sh`, never
    `bash -c "...$VAR..."` or subprocess's `env=` kwarg -- both are
    documented traps on the Windows/MSYS bash this suite also runs under
    (see that test's docstring). `_NEEDS_BASH` already skips this whole
    section there.

    `tmpdir` overrides `TMPDIR` (the guard resolves its transcript log under
    `${TMPDIR:-/tmp}`) -- pass a test-owned, sandboxed directory to avoid
    colliding with real `/tmp` or with other tests running concurrently.
    """
    workdir = tmp_path
    (workdir / "bench-env.sh").write_bytes(ENV.read_bytes())

    setools_dir = workdir / "setools"
    setools_dir.mkdir(exist_ok=True)
    if setools_has_maintenance:
        maint = setools_dir / "maintenance"
        maint.write_text(
            '#!/usr/bin/env bash\n'
            'opt=""\n'
            'prev=""\n'
            'for a in "$@"; do\n'
            '\t[ "$prev" = "-opt" ] && opt="$a"\n'
            '\tprev="$a"\n'
            'done\n'
            'if [ "$opt" = "getbanner" ]; then\n'
            '\tcat "$BANNER_STUB_FILE"\n'
            '\texit "${BANNER_STUB_EXIT:-0}"\n'
            'fi\n'
            'cat "$ATOC_STUB_FILE"\n'
            'exit "${GETTOC_STUB_EXIT:-0}"\n',
            encoding="utf-8",
        )
        maint.chmod(0o755)
    # Absolute paths: the guard now runs `maintenance` via `( cd
    # "$SETOOLS_DIR" && ./maintenance ... )` (matching
    # read-update-log-proof.sh), so a relative stub path would resolve
    # against setools_dir, not workdir.
    stub = workdir / "gettoc.out"
    stub.write_text(gettoc_output or "", encoding="utf-8")
    banner_stub = workdir / "banner.out"
    banner_stub.write_text(
        banner_output if banner_output is not None else _COMPLIANT_BANNER, encoding="utf-8"
    )

    se_uart_line = f'export SE_UART="{se_uart}"\n' if se_uart is not None else ""
    tmpdir_line = f'export TMPDIR="{tmpdir}"\n' if tmpdir is not None else ""
    gate = workdir / "gate.sh"
    gate.write_bytes(
        (
            f"{tmpdir_line}"
            f'export SETOOLS_DIR="{setools_dir.name}"\n'
            f"{se_uart_line}"
            "source ./bench-env.sh\n"
            f'export ATOC_STUB_FILE="{stub}"\n'
            f'export BANNER_STUB_FILE="{banner_stub}"\n'
            f'export GETTOC_STUB_EXIT="{gettoc_exit}"\n'
            f'export BANNER_STUB_EXIT="{banner_exit}"\n'
            f'bench_atoc_replace_guard "{replace_atoc}" {tag} {" ".join(allowed)}\n'
            "exit $?\n"
        ).encode("utf-8")
    )
    return subprocess.run(
        ["bash", "gate.sh"], cwd=workdir, capture_output=True,
        text=True, encoding="utf-8", errors="replace", timeout=60,
    )


@_NEEDS_BASH
def test_atoc_guard_allows_only_its_own_entries(tmp_path):
    """DEVICE plus exactly the caller's own allowed entries -- the ordinary
    post-write state -- must proceed."""
    res = _call_atoc_guard(tmp_path, "0", ["ALP-HE"], "fake-uart", _ONLY_ALLOWED_ATOC)
    assert res.returncode == 0, res.stderr


@_NEEDS_BASH
def test_atoc_guard_allows_a_genuinely_empty_atoc(tmp_path):
    """A fresh/erased board reports 'No ATOC' -- nothing resident to delist,
    so the guard must not treat that as unverified."""
    res = _call_atoc_guard(tmp_path, "0", ["ALP-HE"], "fake-uart", _NO_ATOC)
    assert res.returncode == 0, res.stderr


@_NEEDS_BASH
def test_atoc_guard_aborts_on_a_foreign_resident_entry(tmp_path):
    """The alp-sdk#2025 scenario itself: a resident A32 Linux boot chain that
    is NOT in the caller's allowed set must abort the write, exit 5, and
    name what would be delisted."""
    res = _call_atoc_guard(tmp_path, "0", ["ALP-HE"], "fake-uart", _REAL_MULTI_ENTRY_ATOC)
    assert res.returncode == 5, res.stderr
    for entry in ("BOOTLOAD", "A32_APP", "HP_APP", "HE_APP"):
        assert entry in res.stderr, f"guard did not name {entry} as a delisted entry"


@_NEEDS_BASH
def test_atoc_guard_allows_its_own_multi_entry_write(tmp_path):
    """flash-run-dualcore.sh's own two-entry write (ALP-HP + ALP-HE) must not
    trip the guard on itself -- only a THIRD, foreign entry should."""
    res = _call_atoc_guard(
        tmp_path, "0", ["ALP-HP", "ALP-HE"], "fake-uart", _REAL_MULTI_ENTRY_ATOC
    )
    assert res.returncode == 5, res.stderr
    # HP_APP/HE_APP are resident but named differently from the allowed
    # ALP-HP/ALP-HE this run would write -- still foreign, still refused.
    assert "HP_APP" in res.stderr and "HE_APP" in res.stderr


@_NEEDS_BASH
def test_atoc_guard_replace_atoc_bypasses_a_foreign_entry(tmp_path):
    """--replace-atoc (replace_atoc=1) is the documented, explicit opt-out."""
    res = _call_atoc_guard(tmp_path, "1", ["ALP-HE"], "fake-uart", _REAL_MULTI_ENTRY_ATOC)
    assert res.returncode == 0, res.stderr


@_NEEDS_BASH
def test_atoc_guard_aborts_when_se_uart_is_unset(tmp_path):
    """No SE_UART means the `gettoc` query cannot even be attempted --
    writing blind is exactly the failure mode the guard exists to close
    (this matters for the Flow-D-writing callers, which have no other
    SE_UART dependency)."""
    res = _call_atoc_guard(tmp_path, "0", ["ALP-HE"], None, None)
    assert res.returncode == 5, res.stderr
    assert "SE_UART" in res.stderr or "could not read the resident ATOC" in res.stderr


@_NEEDS_BASH
def test_atoc_guard_replace_atoc_bypasses_an_unverified_query(tmp_path):
    res = _call_atoc_guard(tmp_path, "1", ["ALP-HE"], None, None)
    assert res.returncode == 0, res.stderr


@_NEEDS_BASH
def test_atoc_guard_aborts_when_maintenance_tool_is_missing(tmp_path):
    """SETOOLS_DIR set, SE_UART set, but no `maintenance` binary -- the query
    cannot run and the guard must still refuse rather than assume safety."""
    res = _call_atoc_guard(
        tmp_path, "0", ["ALP-HE"], "fake-uart", None, setools_has_maintenance=False
    )
    assert res.returncode == 5, res.stderr


@_NEEDS_BASH
def test_atoc_guard_aborts_when_gettoc_fails_after_partial_output(tmp_path):
    """alp-sdk#2026 review finding 1: a `maintenance -opt gettoc` that FAILS
    (serial read timeout mid-table) after emitting only two `DEVICE` rows
    must not decode as a verified, all-clear query from the transcript text
    alone -- the exit status has to force `unverified`, not just the text."""
    res = _call_atoc_guard(
        tmp_path,
        "0",
        ["ALP-HE"],
        "fake-uart",
        "|   DEVICE |  CM0+  | 0x8057C6F0 | 0x8057BCF0 | ---------- | ---------- |"
        "      312 |  0.5.0| u V  |\n"
        "|   DEVICE |  CM0+  | 0x805C1EC0 | 0x805C14C0 | ---------- | ---------- |"
        "      372 |  0.5.0| u V  |\n"
        "ERROR: Target did not respond\n",
        gettoc_exit=1,
    )
    assert res.returncode == 5, (
        f"a gettoc that exited 1 after partial rows must abort, got {res.returncode}\n"
        f"{res.stdout}{res.stderr}"
    )


@_NEEDS_BASH
def test_atoc_guard_allows_an_ansi_coloured_compliant_table(tmp_path):
    """alp-sdk#2026 review finding 3: SETOOLS colours its own output on some
    terminals/versions. A compliant table (DEVICE + the caller's own
    ALP-HE) wrapped in ANSI SGR codes must still parse as compliant -- not
    misread the coloured names as foreign and abort on the guard's own
    legitimate output."""
    ansi_table = (
        "|   \x1b[32mDEVICE\x1b[0m |  CM0+  | 0x8057C6F0 | 0x8057BCF0 | ---------- |"
        " ---------- |      312 |  0.5.0| u V  |\n"
        "|   \x1b[32mALP-HE\x1b[0m | M55-HE | 0x8057EDB0 | 0x8057E3B0 | 0x58000000 |"
        " 0x58000000 |     4480 |  1.0.0| uLVB |\n"
    )
    res = _call_atoc_guard(tmp_path, "0", ["ALP-HE"], "fake-uart", ansi_table)
    assert res.returncode == 0, (
        f"an ANSI-coloured but compliant table must pass, got {res.returncode}\n"
        f"{res.stdout}{res.stderr}"
    )


_BEFORE_LOG_RE = re.compile(r"resident ATOC before this write \((?P<path>[^)]+)\)")


def _before_log_path(res: subprocess.CompletedProcess[str]) -> str:
    m = _BEFORE_LOG_RE.search(res.stderr)
    assert m, f"guard did not print a transcript path\n{res.stderr}"
    return m.group("path")


@_NEEDS_BASH
def test_atoc_guard_transcript_path_is_run_unique(tmp_path):
    """alp-sdk#2026 round-2 review BLOCKER: `tag` is a literal script name
    (flash-run, flash-run-dualcore, ...), not run-unique. Three AEN boards
    on this farm (evk-01/-02/-03) makes two concurrent runs of the SAME
    script against DIFFERENT boards a real scenario, and a shared fixed
    path let one run's write land between another run's redirect and read
    -- reproduced: the second run printed the first run's clean table and
    returned rc=0 on a board that actually carried a foreign A32_APP/HE_APP.
    Two sequential invocations with the IDENTICAL tag and TMPDIR must land
    on two DIFFERENT transcript paths."""
    first = _call_atoc_guard(tmp_path, "0", ["ALP-HE"], "fake-uart", _ONLY_ALLOWED_ATOC)
    second = _call_atoc_guard(tmp_path, "0", ["ALP-HE"], "fake-uart", _ONLY_ALLOWED_ATOC)
    assert first.returncode == 0, first.stderr
    assert second.returncode == 0, second.stderr
    assert _before_log_path(first) != _before_log_path(second), (
        "two runs of the same script (same tag) must not share a transcript path -- "
        "that shared path is exactly the alp-sdk#2025-through-a-new-door hazard"
    )


@_NEEDS_BASH
def test_atoc_guard_aborts_when_tmpdir_is_unwritable(tmp_path):
    """Rescoped from the round-1 fix's stale-log test: that test's own
    docstring claim ("read-only, which is enough to make the redirect fail
    the same way") is false -- `rm -f` on a 0o444 file you own in a
    writable directory succeeds (unlink only checks the DIRECTORY's write
    bit, never the file's own permissions), so the pre-fix guard's `rm -f
    ... || return 5` line was never actually exercised by it; deleting that
    line left all `-k atoc` tests green. `mktemp` (this fix) makes the
    per-run path collision-proof by construction instead, so the genuine
    unremovable-target failure mode is now "the directory itself refuses a
    new file" -- cover THAT directly with an unwritable directory.

    Round-3 review MAJOR: an earlier version of this test asserted only
    `returncode == 5`, which does NOT distinguish this fix from either a
    mutated `|| true` on the `mktemp` line (measured: still rc=5, from an
    unrelated downstream redirect failure) or from round-2's pre-mktemp
    code (101368cdf, fixed path + `rm -f`: also rc=5, via a bare "Permission
    denied" from bash's own `>` redirect, never this guard's own message).
    Assert the CAUSE: this fix's specific `mktemp`-failure abort message,
    not present in either of those."""
    unwritable = tmp_path / "unwritable-tmp"
    unwritable.mkdir()
    unwritable.chmod(0o500)
    try:
        res = _call_atoc_guard(
            tmp_path, "0", ["ALP-HE"], "fake-uart", _ONLY_ALLOWED_ATOC, tmpdir=unwritable
        )
        assert res.returncode == 5, (
            f"an unwritable TMPDIR must abort, not silently skip the transcript, "
            f"got {res.returncode}\n{res.stdout}{res.stderr}"
        )
        assert "cannot create the pre-write ATOC transcript" in res.stderr, (
            f"must abort specifically because mktemp itself failed, not some "
            f"unrelated downstream cause -- {res.stderr}"
        )
    finally:
        unwritable.chmod(0o700)


# --- validated against REAL silicon captures off e1m-aen-evk-01 (2026-09-07),
# ANSI intact, not synthesised (alp-sdk#2026 round-2 review) -----------------


@_NEEDS_BASH
def test_atoc_guard_accepts_the_real_getbanner_capture(tmp_path):
    """The real `getbanner` line is " SES A1 v1.110.0 Mar  4 2026 19:06:23"
    (ANSI-stripped) -- note the LEADING SPACE, which is SETOOLS' own
    padding, not a terminal artifact. A bare `^SES` anchor rejected this
    exact line, aborting every run on real hardware regardless of what
    gettoc reported."""
    res = _call_atoc_guard(
        tmp_path,
        "0",
        ["ALP-HE"],
        "fake-uart",
        _ONLY_ALLOWED_ATOC,
        banner_output=_REAL_GETBANNER,
    )
    assert res.returncode == 0, (
        f"the real getbanner capture must be accepted, got {res.returncode}\n"
        f"{res.stdout}{res.stderr}"
    )


@_NEEDS_BASH
def test_atoc_guard_banner_exit_status_is_not_discarded(tmp_path):
    """MAJOR: only the banner TEXT was checked, never getbanner's own exit
    status. Measured: getbanner exiting 3 while still printing a
    well-formed, compliant banner line let the query proceed and returned
    rc=0. A non-zero getbanner must force the query unverified, same class
    as the gettoc rc fix from round 1."""
    res = _call_atoc_guard(
        tmp_path,
        "0",
        ["ALP-HE"],
        "fake-uart",
        _ONLY_ALLOWED_ATOC,
        banner_output=_COMPLIANT_BANNER,
        banner_exit=3,
    )
    assert res.returncode == 5, (
        f"a non-zero getbanner exit must abort even with a valid banner line, "
        f"got {res.returncode}\n{res.stdout}{res.stderr}"
    )


@_NEEDS_BASH
def test_atoc_guard_parses_a_real_ansi_coloured_9row_transcript(tmp_path):
    """The exact alp-sdk#2025 incident shape, captured for real: SETOOLS
    colours the WHOLE LINE (`\\x1b[94m |...|`, closing `\\x1b[0m` on the
    NEXT line, not the same one), so a table-row match anchored to a bare
    leading `|` never matched a single real row -- `resident` silently
    computed empty on every real coloured transcript. Must abort (rc=5)
    and name exactly the four foreign app entries -- never SERAM0/SERAM1
    (SE firmware, not app state) or the duplicate DEVICE rows."""
    res = _call_atoc_guard(tmp_path, "0", ["ALP-HE"], "fake-uart", _REAL_GETTOC_9ROW)
    assert res.returncode == 5, (
        f"the real 9-row incident capture must abort, got {res.returncode}\n"
        f"{res.stdout}{res.stderr}"
    )
    # Must abort via the "this write REPLACES" foreign-entry path, i.e. the
    # table was actually PARSED as containing foreign entries -- not the
    # unrelated "could not read the resident ATOC" unverified path, which a
    # broken parser (empty `resident`) would also land on with rc=5 while
    # still dumping the raw (unparsed) transcript text -- including the
    # literal substring "BOOTLOAD" -- to stderr. Anchoring only to rc==5 and
    # substring presence would pass against the pre-fix `/^\|/` anchor too.
    assert "this write REPLACES" in res.stderr, (
        f"guard aborted for the wrong reason (parser likely found ZERO rows -- "
        f"the pre-fix `/^\\|/` anchor bug) -- {res.stderr}"
    )
    for entry in ("BOOTLOAD", "A32_APP", "HP_APP", "HE_APP"):
        assert entry in res.stderr, f"guard did not name {entry} as a delisted entry"
    assert "SERAM0" not in res.stderr.split("also carries:", 1)[-1].split("\n")[0], (
        "SERAM0 is SE firmware, never touched by app-write-mram -p -- "
        "it must not be named as a would-be-delisted entry"
    )


@_NEEDS_BASH
def test_atoc_guard_parses_a_real_ansi_coloured_clean_transcript(tmp_path):
    """The same board's real, CLEAN, compliant state: only DEVICE + its own
    ALP-HE, plus the two baseline SERAM0/SERAM1 banks that are resident on
    every real board regardless of the last app write. Without treating
    SERAM0/SERAM1 as baseline (like DEVICE), this exact real, compliant
    capture aborted unconditionally -- the guard would never pass on real
    hardware."""
    res = _call_atoc_guard(tmp_path, "0", ["ALP-HE"], "fake-uart", _REAL_GETTOC_CLEAN)
    assert res.returncode == 0, (
        f"the real clean capture must pass, got {res.returncode}\n"
        f"{res.stdout}{res.stderr}"
    )


@_NEEDS_BASH
def test_atoc_guard_seram_exemption_checks_the_cpu_column(tmp_path):
    """Round-3 review MINOR: the DEVICE/SERAM0/SERAM1 baseline exemption
    matched the Name cell alone, with no CPU-column cross-check. Measured
    silent-pass: a row rendered `|   SERAM1 | M55-HE | ...` (a real app
    entry that merely collides with the baseline SE-firmware bank's name)
    was silently exempted -- rc=0 -- exactly like a genuine SERAM1 row.
    Real captures show every baseline row as `CM0+`; no app entry ever is.
    Gate the exemption on the CPU column too, so a same-named row on a
    DIFFERENT core still trips the guard."""
    res = _call_atoc_guard(
        tmp_path,
        "0",
        ["ALP-HE"],
        "fake-uart",
        "|   DEVICE |  CM0+  | 0x8057C6F0 | 0x8057BCF0 | ---------- | ---------- |"
        "      312 |  0.5.0| u V  |\n"
        "|   SERAM1 | M55-HE | 0x80565070 | 0x80564670 | 0x58000000 | 0x58000000 |"
        "   110356 |  1.0.0| uLVB |\n",
    )
    assert res.returncode == 5, (
        f"a SERAM1-named row on M55-HE (not CM0+) is a real app entry, not SE "
        f"firmware -- it must trip the guard, got {res.returncode}\n"
        f"{res.stdout}{res.stderr}"
    )
    assert "SERAM1" in res.stderr


def test_atoc_guard_mktemp_template_has_trailing_x_placeholder() -> None:
    """CI caught this, the Linux-only local suite did not: `mktemp
    ".../${tag}-atoc-before.XXXXXX.log"` has the X's in the MIDDLE (a
    literal ".log" after them). GNU mktemp tolerates that; BSD/macOS
    mktemp requires the placeholder to be the literal END of the template
    and fails EVERY call with a misleading "File exists" -- measured on
    macOS CI (`python-smoke (macos-latest)`): the guard aborted (exit 5)
    on every single run, dead on that platform, in the fail-closed
    direction.

    This is a STRUCTURAL check, not a behavioural one -- there is no
    BSD/macOS mktemp to actually run here (this suite is Linux-only), so
    this cannot prove the guard now WORKS on macOS, only that the specific
    template shape that broke it cannot silently come back. A real
    macOS run of this suite (CI) is what actually proves the fix; treat
    this test as a tripwire against re-introducing a mid-template
    placeholder, not as macOS coverage."""
    body = ENV.read_text(encoding="utf-8")
    m = re.search(r'mktemp\s+"[^"]*atoc-before\.([A-Za-z.]+)"', body)
    assert m, "could not find the atoc-before mktemp call in bench-env.sh"
    placeholder = m.group(1)
    assert placeholder == "X" * 6, (
        f"the mktemp template's X-placeholder must be exactly 6 trailing X's "
        f"with NOTHING after them (BSD/macOS mktemp requires this) -- got "
        f"{placeholder!r}"
    )


def test_every_atoc_committing_script_calls_the_shared_guard() -> None:
    """The four scripts known to commit a fresh ATOC must route through the
    ONE shared guard, not a hand-rolled copy -- that drift is exactly what
    alp-sdk#2025's follow-up closed."""
    for script in (
        "flash-run.sh",
        "flash-run-dualcore.sh",
        "flash-update-log-dual.sh",
        "flash-update-log-firewall-probe.sh",
    ):
        body = (BENCH / script).read_text(encoding="utf-8")
        assert "bench_atoc_replace_guard" in body, f"{script} does not call the shared ATOC guard"
