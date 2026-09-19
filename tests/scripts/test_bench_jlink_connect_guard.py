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
import tempfile
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BENCH = REPO / "scripts" / "bench" / "aen"
ENV = BENCH / "bench-env.sh"


def _sanitized_env() -> dict[str, str]:
    """alp-sdk#2064 review, Major 1: a unit test must NEVER be able to reach
    real bench infrastructure. Every call site below sources bench-env.sh,
    which -- if LG_PLACE is set (an operator's own shell profile, exported
    for OTHER work in the same session) -- eagerly resolves it against the
    REAL labgrid coordinator the moment it is sourced, live, over the
    network, regardless of what the test itself goes on to do. Strip every
    input that could route bench-env.sh/bench_jlink_run() at real
    infrastructure or a real probe; inheriting os.environ verbatim is
    exactly what would otherwise leak these in."""
    env = dict(os.environ)
    for var in ("LG_PLACE", "LG_COORDINATOR", "LG_SWD_PATH", "ALP_JLINK_SEARCH_ROOT"):
        env.pop(var, None)
    return env


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
            capture_output=True, text=True, encoding="utf-8", timeout=30,
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


def _dir_still_accepts_a_new_file(directory: Path) -> bool:
    """True when `directory` took a new entry despite having been chmod'ed
    read-only -- i.e. the "unwritable directory" precondition did NOT hold.

    Same discipline as _bash_can_run_a_script() above: probe by DOING the
    thing, never by trusting the platform to honour the mode bits. Two hosts
    hand back a writable directory after a restrictive chmod, for different
    reasons, and only a real create tells them apart from a genuinely
    refusing one:

      * Windows -- `os.chmod` only toggles FILE_ATTRIBUTE_READONLY, which
        does not stop a directory accepting new entries. A `chmod(0o500)`
        directory reads back 0o555, `os.access(W_OK)` returns True, and
        `mktemp` inside it succeeds.
      * POSIX as root (or with CAP_DAC_OVERRIDE) -- the chmod DID take, and
        the caller bypasses it anyway.

    A test that needs the directory to refuse a write has no precondition on
    either host and must skip, not fail (alp-sdk#2055).

    Probe with `tempfile.mkstemp(dir=...)` rather than `Path.touch()`: touch
    defaults to `exist_ok=True`, whose fast path is a bare `os.utime` on an
    existing file -- and a file's own mtime does not need the DIRECTORY's
    write bit, so a leftover probe would report "writable" without ever
    attempting a create. mkstemp always creates, and creates with the same
    syscall shape as the `mktemp` under test.
    """
    try:
        fd, made = tempfile.mkstemp(dir=directory, prefix=".alp-writability-probe")
    except OSError:
        return False
    os.close(fd)
    try:
        os.unlink(made)
    except OSError:
        pass
    return True


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
        # `unset`, not just the `env=` kwarg below -- see _sanitized_env()'s
        # docstring and the AEN_DPIDR override test further down, which
        # documents `env=` not reliably reaching an MSYS bash on some hosts.
        "unset LG_PLACE LG_COORDINATOR LG_SWD_PATH ALP_JLINK_SEARCH_ROOT; "
        'source ./bench-env.sh; '
        f'bench_jlink_assert_connected "{out_file.name}" "unit-test"'
    )
    return subprocess.run(
        ["bash", "-c", script],
        cwd=workdir,
        env=_sanitized_env(),
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
    # ...and be given the actionable next step, verbatim (alp-sdk#2064: the
    # hint now names LG_PLACE -- the actual selector every helper routes
    # through via bench_jlink_run() -- not the dead JLINK_SN).
    assert "export LG_PLACE=" in res.stderr
    assert "JLINK_SN" not in res.stderr, "must not send operators back to the dead serial-only selector"


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
#
# Two output-path SHAPES (alp-sdk#2233 review round 3, finding 9): a bare
# `/tmp/foo.out` literal (scripts not yet touched by the TMPDIR conversion),
# or a QUOTED `"${TMPDIR:-/tmp}/foo.out"` (the six Flow D writers, converted
# so concurrent pytest runs against the same host /tmp no longer collide).
_READBACK_RE = re.compile(
    r'^[ \t]*\S.*-CommanderScript\s.*?>\s*(?P<out>"\$\{TMPDIR:-/tmp\}/[^"]+"|/tmp/\S+)\s*\|\|\s*true[ \t]*$',
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
        "unset LG_PLACE LG_COORDINATOR LG_SWD_PATH ALP_JLINK_SEARCH_ROOT; "
        'source ./bench-env.sh; '
        f'bench_jlink_assert_aen_dpidr "{out_file.name}" "unit-test"'
    )
    return subprocess.run(
        ["bash", "-c", script], cwd=workdir, env=_sanitized_env(), capture_output=True,
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
            # alp-sdk#2064 review, Major 1: `unset` inside the script itself,
            # not just the `env=` kwarg below -- this test's own docstring
            # above documents `env=` NOT reliably reaching this host's MSYS
            # bash at all, so a literal `unset` is the only reliable way to
            # keep an inherited LG_PLACE from making bench-env.sh resolve
            # against a real coordinator the moment it is sourced.
            "unset LG_PLACE LG_COORDINATOR LG_SWD_PATH ALP_JLINK_SEARCH_ROOT\n"
            'AEN_DPIDR="DEADBEEF"\n'          # the attempted override
            "source ./bench-env.sh\n"
            f'bench_jlink_assert_aen_dpidr "{out.name}" "unit-test"\n'
        ).encode("utf-8")
    )
    res = subprocess.run(
        ["bash", "gate.sh"], cwd=workdir, env=_sanitized_env(), capture_output=True,
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


# --- alp-sdk#2233: `verifybin` retired as a gate, replaced by a fresh-session
# savebin read-back proof against a sector-padded image -------------------
#
# alp-sdk#1488/#1343 (superseded, this section used to test them directly)
# taught these six scripts to gate on `verifybin`'s outcome. alp-sdk#2233
# then measured that the gate itself proved nothing: `verifybin` compares
# against J-Link's own in-process flash CACHE, never a fresh chip read, so
# `Verify successful.` was true even when MRAM held something else. The fix
# retired `verifybin` as a gate everywhere (scripts/bench/aen/flash-jlink.sh,
# flash-jlink-hp.sh, flash-jlink-mramxip.sh, flash-update-log-dual.sh,
# flash-update-log-firewall-probe.sh, erase-storage.sh) in favour of
# bench_flowd_proof() (bench-env.sh): a FRESH read-only J-Link session that
# `savebin`s the written range back and `cmp`s it against a padded image that
# also proves the write's sector NEIGHBOURS survived (the loader rewrites
# whole 16 KiB sectors and never reads their prior contents -- the other half
# of #2233, see scripts/bench/aen/flowd_sector_pad.py and
# tests/scripts/test_flowd_sector_pad.py).
#
# This section replaces the old alp-sdk#1488 "did the gate change the exit
# status" derivation with the equivalent regression tests for the new gate:
# no script may reintroduce a `verifybin`-outcome gate, and every former
# verifybin writer must call bench_flowd_proof() and embed
# bench_flowd_loadbin_lines' padded loadbin line(s) before treating a write
# as done.

# A `verifybin`-outcome GATE, specifically -- an `if` whose condition greps a
# transcript for "verify failed"/"verify successful". A bare `verifybin`
# COMMAND left in a CommanderScript purely for its log value (explicitly
# allowed by alp-sdk#2233) is NOT what this matches; only a live gate built
# on its output is.
_VERIFY_GATE_RE = re.compile(
    r'^[ \t]*if\s+.*grep[^\n]*"verify (?:failed|successful)"', re.M
)


def test_no_script_gates_on_verifybin_any_more() -> None:
    """Regression test for alp-sdk#2233: `verifybin`'s outcome must never
    decide a script's exit status again, on any bench script -- not just the
    six known sites. A `Verify successful.`/`Verify failed.` string is still
    fine in a display-only grep (the `grep -iE "...|Verify|O\\.K\\...."`
    lines these scripts print for a human to read stay); what must never
    come back is an `if` whose CONDITION is that grep.
    """
    offenders = [
        p.name for p in _bench_scripts() if _VERIFY_GATE_RE.search(p.read_text(encoding="utf-8"))
    ]
    assert not offenders, (
        "verifybin is gating a script again (alp-sdk#2233 regression) -- "
        "replace it with bench_flowd_proof(): " + ", ".join(offenders)
    )


# Every script that used to gate on verifybin now calls bench_flowd_proof()
# instead. Anchored on the function name with its second (manifest) argument,
# which is how every call site actually reads -- a bare substring match on
# "bench_flowd_proof" would also match this file's own comments.
_FLOWD_PROOF_CALL_RE = re.compile(r'bench_flowd_proof\s+\S+\s+"\$FLOWD_MANIFEST"')

_FLOWD_PROOF_SCRIPTS = (
    "flash-jlink.sh",
    "flash-jlink-hp.sh",
    "flash-jlink-mramxip.sh",
    "flash-update-log-dual.sh",
    "flash-update-log-firewall-probe.sh",
    "erase-storage.sh",
)


@pytest.mark.parametrize("script", _FLOWD_PROOF_SCRIPTS)
def test_every_former_verifybin_writer_now_calls_bench_flowd_proof(script: str) -> None:
    """The six scripts alp-sdk#1488/#1343/#2233 have touched for this defect
    must call the current gate, not a dangling reference to the retired one."""
    body = (BENCH / script).read_text(encoding="utf-8")
    assert _FLOWD_PROOF_CALL_RE.search(body), (
        f'{script} does not call bench_flowd_proof(<tag>, "$FLOWD_MANIFEST", ...) -- '
        "the alp-sdk#2233 read-back proof that replaced verifybin"
    )
    assert "bench_flowd_prepare_write" in body or "bench_flowd_build" in body, (
        f"{script} calls bench_flowd_proof but never builds a padded manifest "
        "via bench_flowd_prepare_write/bench_flowd_build first"
    )


def test_every_former_verifybin_writer_embeds_padded_loadbin_lines() -> None:
    """The raw `loadbin <blob> <address>` these scripts used to write by hand
    must be gone, replaced by bench_flowd_loadbin_lines' output -- otherwise
    the padding built above is computed and then never actually used."""
    missing = [
        p.name for p in _bench_scripts()
        if p.name in _FLOWD_PROOF_SCRIPTS and "bench_flowd_loadbin_lines" not in p.read_text(encoding="utf-8")
    ]
    assert not missing, f"script(s) build a padded manifest but never embed it via bench_flowd_loadbin_lines: {missing}"


# alp-sdk#2233 review major 5, mutation M4: a RAW, unpadded `loadbin <blob>
# <address>` line reintroduced alongside (or instead of) the embedded
# bench_flowd_loadbin_lines output. None of the six scripts should ever
# contain a literal `loadbin` command word any more -- every one now comes
# from the padded variable at RUNTIME, invisible to a static grep of the
# SOURCE text. A literal `loadbin ` word in the source (not inside a comment
# or this file's own regex) is therefore always a regression.
_RAW_LOADBIN_LINE_RE = re.compile(r"^[ \t]*loadbin[ \t]", re.M)


def test_no_script_has_a_raw_loadbin_line_outside_the_embedded_variable() -> None:
    """alp-sdk#2233 review major 5 (mutation M4). Every loadbin now comes
    from `$FLOWD_LOADBIN_LINES`/`$FLOWD_PREWRITE_LINES`, expanded at
    runtime -- a literal `loadbin <arg> <arg>` line reappearing in a
    script's own source is always the unpadded-write regression #2233
    fixed, whether reintroduced instead of or alongside the padded one."""
    offenders = {}
    for p in _bench_scripts():
        if p.name not in _FLOWD_PROOF_SCRIPTS:
            continue
        body = p.read_text(encoding="utf-8")
        hits = _RAW_LOADBIN_LINE_RE.findall(body)
        if hits:
            offenders[p.name] = len(hits)
    assert not offenders, f"raw (unpadded) loadbin line(s) found in: {offenders}"


# alp-sdk#2233 review major 5, mutations M3/M6: the proof gate's polarity.
# Every `bench_flowd_proof` call site must be the CONDITION of an `if !`
# (fail-closed) -- inverting it (M3) or short-circuiting it with `if false
# && !` (M6) both defeat the gate while a bare substring search for
# "bench_flowd_proof" (test_every_former_verifybin_writer_now_calls_bench_flowd_proof
# above) would not notice either. Complements (does not replace) the
# behavioural mutation-proof in test_flowd_writer_scripts_e2e.py.
_FLOWD_PROOF_NEGATED_RE = re.compile(r"^[ \t]*if[ \t]+![ \t]*bench_flowd_proof\b", re.M)


@pytest.mark.parametrize("script", _FLOWD_PROOF_SCRIPTS)
def test_bench_flowd_proof_call_is_the_condition_of_a_negated_if(script: str) -> None:
    body = (BENCH / script).read_text(encoding="utf-8")
    assert _FLOWD_PROOF_NEGATED_RE.search(body), (
        f"{script}: bench_flowd_proof is not called as `if ! bench_flowd_proof ...` -- "
        "an inverted or short-circuited gate would defeat it silently"
    )


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

# Real captures off an AEN EVK bench unit, 2026-09-07, ANSI intact -- verbatim,
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
            "unset LG_PLACE LG_COORDINATOR LG_SWD_PATH ALP_JLINK_SEARCH_ROOT\n"
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
        ["bash", "gate.sh"], cwd=workdir, env=_sanitized_env(), capture_output=True,
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
    on this farm makes two concurrent runs of the SAME
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
    not present in either of those.

    Only runs where an unwritable directory can actually be created: see
    _dir_still_accepts_a_new_file() and alp-sdk#2055 for why Windows cannot
    establish that precondition and skips instead."""
    unwritable = tmp_path / "unwritable-tmp"
    unwritable.mkdir()
    unwritable.chmod(0o500)
    try:
        if _dir_still_accepts_a_new_file(unwritable):
            _euid = getattr(os, "geteuid", lambda: None)()
            pytest.skip(
                "this directory did not refuse a new entry after chmod(0o500) "
                f"(os.name={os.name!r}, euid={_euid!r}), so the unwritable-TMPDIR "
                "precondition does not hold here -- on Windows because chmod "
                "cannot make a directory refuse an entry at all, under a POSIX "
                "root/CAP_DAC_OVERRIDE caller because the mode took and was "
                "bypassed. See alp-sdk#2055; "
                "test_atoc_guard_aborts_when_tmpdir_does_not_exist covers the "
                "same mktemp-failure branch on every host."
            )
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


@_NEEDS_BASH
def test_atoc_guard_aborts_when_tmpdir_does_not_exist(tmp_path):
    """The same `mktemp`-failure branch as the unwritable-TMPDIR test above,
    reached by a precondition that holds on EVERY host.

    That test can only build its precondition where `chmod` actually makes a
    directory refuse an entry, so it skips on Windows and under a POSIX root
    caller (alp-sdk#2055). A TMPDIR that does not exist drives the identical
    `mktemp ... || return 5` line in bench-env.sh's
    bench_atoc_replace_guard(), needs no mode bits to do it, and therefore
    keeps that branch covered on hosts where the chmod route is unavailable.
    Assert the same CAUSE, not merely `returncode == 5`: this guard's own
    mktemp-failure message, which neither a mutated `|| true` on the mktemp
    line nor the pre-mktemp fixed-path code ever printed.
    """
    missing = tmp_path / "no-such-dir" / "nor-this-one"
    assert not missing.exists(), "the precondition is that this path is absent"

    res = _call_atoc_guard(
        tmp_path, "0", ["ALP-HE"], "fake-uart", _ONLY_ALLOWED_ATOC, tmpdir=missing
    )
    assert res.returncode == 5, (
        f"a TMPDIR that does not exist must abort, not silently skip the "
        f"transcript, got {res.returncode}\n{res.stdout}{res.stderr}"
    )
    assert "cannot create the pre-write ATOC transcript" in res.stderr, (
        f"must abort specifically because mktemp itself failed, not some "
        f"unrelated downstream cause -- {res.stderr}"
    )


# --- validated against REAL silicon captures off an AEN EVK bench unit (2026-09-07),
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
    placeholder, not as macOS coverage.

    alp-sdk#2233 re-introduced the same shape in seven more templates
    (`flowd-read-XXXXXX.jlink`, `aen-erase-atoc-trailer-XXXXXX.bin`, ...),
    and macOS CI went red again, so the tripwire now sweeps every mktemp
    call in every bench script rather than the one template that broke
    first."""
    body = ENV.read_text(encoding="utf-8")
    m = re.search(r'mktemp\s+"[^"]*atoc-before\.([A-Za-z.]+)"', body)
    assert m, "could not find the atoc-before mktemp call in bench-env.sh"
    placeholder = m.group(1)
    assert placeholder == "X" * 6, (
        f"the mktemp template's X-placeholder must be exactly 6 trailing X's "
        f"with NOTHING after them (BSD/macOS mktemp requires this) -- got "
        f"{placeholder!r}"
    )
    mid_template = []
    for sh in sorted(BENCH.parent.rglob("*.sh")):
        for n, line in enumerate(sh.read_text(encoding="utf-8").splitlines(), 1):
            if not line.lstrip().startswith("#") and re.search(r'\bmktemp\b[^#]*X{3,}[^X\s"\')]', line):
                mid_template.append(f"{sh.relative_to(REPO)}:{n}: {line.strip()}")
    assert not mid_template, (
        "mktemp templates with a suffix after the X's fail on BSD/macOS mktemp:\n"
        + "\n".join(mid_template)
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


# --- alp-sdk#2027: bench_flowd_atoc_guard, the Flow D (no-SE-UART-by-design)
# follow-up to bench_atoc_replace_guard above -----------------------------
#
# flash-jlink.sh / flash-jlink-hp.sh / flash-jlink-mramxip.sh write the same
# replacing ATOC over SWD that Flow A writes over $SE_UART, but Flow D's
# whole premise is "J-Link only, no serial device required" (#2025/#2026),
# so PR #2029 made an EXPLICIT --atoc-unqueryable acknowledgement the interim
# fix instead of wiring the SE_UART-only guard in unconditionally (which
# would have made SE_UART a hard requirement of Flow D -- the one thing this
# issue exists to avoid). bench_flowd_atoc_guard is the follow-up that closes
# the other half: when $SE_UART happens to be exported and usable, it is no
# longer optional whether the resident ATOC gets checked.


def _call_flowd_guard(
    tmp_path: Path,
    replace_atoc: str,
    unqueryable: str,
    allowed: list[str],
    se_uart: str | None,
    gettoc_output: str | None,
    setools_has_maintenance: bool = True,
    tag: str = "flowd-test",
    tmpdir: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run bench_flowd_atoc_guard (bench-env.sh, alp-sdk#2027) against a
    synthetic `maintenance -opt gettoc` transcript, without touching any
    real bench, SETOOLS install, or probe.

    Same file-based-script / stub-maintenance discipline as
    `_call_atoc_guard` above -- a compliant `getbanner` is hardcoded into the
    stub (this suite's SE_UART-present cases care about the gettoc table,
    not the banner-parsing edge cases `_call_atoc_guard`'s own tests already
    cover in isolation).
    """
    workdir = tmp_path
    (workdir / "bench-env.sh").write_bytes(ENV.read_bytes())

    setools_dir = workdir / "setools-flowd"
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
            '\tprintf "%s\\n" "SES A1 v1.110.0 Mar  4 2026 19:06:23"\n'
            '\texit 0\n'
            'fi\n'
            'cat "$ATOC_STUB_FILE"\n'
            'exit 0\n',
            encoding="utf-8",
        )
        maint.chmod(0o755)
    stub = workdir / "flowd-gettoc.out"
    stub.write_text(gettoc_output or "", encoding="utf-8")

    se_uart_line = f'export SE_UART="{se_uart}"\n' if se_uart is not None else ""
    tmpdir_line = f'export TMPDIR="{tmpdir}"\n' if tmpdir is not None else ""
    gate = workdir / "flowd-gate.sh"
    gate.write_bytes(
        (
            f"{tmpdir_line}"
            f'export SETOOLS_DIR="{setools_dir.name}"\n'
            f"{se_uart_line}"
            "source ./bench-env.sh\n"
            f'export ATOC_STUB_FILE="{stub}"\n'
            f'bench_flowd_atoc_guard "{replace_atoc}" "{unqueryable}" {tag} {" ".join(allowed)}\n'
            "exit $?\n"
        ).encode("utf-8")
    )
    return subprocess.run(
        ["bash", "flowd-gate.sh"], cwd=workdir, capture_output=True,
        text=True, encoding="utf-8", errors="replace", timeout=60,
    )


@_NEEDS_BASH
def test_flowd_guard_with_se_uart_runs_the_shared_guard_and_refuses(tmp_path):
    """SE_UART exported: alp-sdk#2027's whole point -- Flow D must run the
    SAME query-based guard Flow A uses, not a second, weaker one. A resident
    A32 Linux boot chain not in the caller's allowed set must abort exactly
    like Flow A does (exit 5), naming what would be delisted. Passing
    --atoc-unqueryable alongside must NOT be a way around this -- it is the
    no-SE-UART acknowledgement, and a real check just ran."""
    res = _call_flowd_guard(
        tmp_path, "0", "1", ["ALP-HE"], "fake-uart", _REAL_MULTI_ENTRY_ATOC
    )
    assert res.returncode == 5, res.stderr
    assert "this write REPLACES" in res.stderr
    for entry in ("BOOTLOAD", "A32_APP", "HP_APP", "HE_APP"):
        assert entry in res.stderr, f"guard did not name {entry} as a delisted entry"
    assert "proceeding WITHOUT the resident-ATOC check" not in res.stderr, (
        "--atoc-unqueryable must not silence a real, SE_UART-backed check"
    )


@_NEEDS_BASH
def test_flowd_guard_with_se_uart_and_clean_board_proceeds(tmp_path):
    """SE_UART exported and the board carries only DEVICE + the caller's own
    entry: the real query passes, exactly as it would for Flow A."""
    res = _call_flowd_guard(
        tmp_path, "0", "0", ["ALP-HE"], "fake-uart", _ONLY_ALLOWED_ATOC
    )
    assert res.returncode == 0, res.stderr


@_NEEDS_BASH
def test_flowd_guard_with_se_uart_replace_atoc_bypasses_a_foreign_entry(tmp_path):
    """--replace-atoc still works as the explicit override in the SE_UART
    branch, exactly as it does for Flow A: Flow D gains the SAME capability
    Flow A has here, not a separate one."""
    res = _call_flowd_guard(
        tmp_path, "1", "0", ["ALP-HE"], "fake-uart", _REAL_MULTI_ENTRY_ATOC
    )
    assert res.returncode == 0, res.stderr


@_NEEDS_BASH
def test_flowd_guard_without_se_uart_and_without_flag_aborts(tmp_path):
    """SE_UART unset, no --atoc-unqueryable: the #2029 interim behaviour must
    be unchanged -- abort (exit 8), not fail some other way and not silently
    proceed. The message must name BOTH ways forward (export SE_UART so the
    guard above can run for real, or pass the flag), never just one."""
    res = _call_flowd_guard(tmp_path, "0", "0", ["ALP-HE"], None, None)
    assert res.returncode == 8, res.stderr
    assert "REFUSING TO WRITE" in res.stderr
    assert "export SE_UART=" in res.stderr, "must name exporting SE_UART as a way forward"
    assert "--atoc-unqueryable" in res.stderr, "must name the flag as the other way forward"


@_NEEDS_BASH
def test_flowd_guard_without_se_uart_and_with_flag_proceeds_and_says_so(tmp_path):
    """SE_UART unset, --atoc-unqueryable passed: proceed (exit 0), and print
    the one-line statement that the resident-ATOC check did NOT run and why
    -- a skipped check must never read like a passed one."""
    res = _call_flowd_guard(tmp_path, "0", "1", ["ALP-HE"], None, None)
    assert res.returncode == 0, res.stderr
    assert "SE_UART is unset -- proceeding WITHOUT the resident-ATOC check" in res.stderr
    assert "--atoc-unqueryable" in res.stderr


@_NEEDS_BASH
def test_flowd_guard_aborts_when_maintenance_tool_is_missing_despite_se_uart(tmp_path):
    """SE_UART set but no `maintenance` binary in SETOOLS_DIR: the query
    cannot actually run, so this must land in bench_atoc_replace_guard's own
    unverified path (exit 5), not silently pass as if SE_UART merely being
    set were the whole check.

    Review MINOR 2: `bench_atoc_replace_guard` returns 5 from FIVE distinct
    sites (the `mktemp` failure, two `echo ... >"$before" || return 5`
    writes, the `[ -f "$before" ]` check, and the intended unverified-query
    abort) -- `test_atoc_guard_aborts_when_tmpdir_is_unwritable` above already
    demonstrates the `mktemp` path firing on this host for an UNRELATED
    reason. Asserting only `returncode == 5` here cannot tell "the guard
    correctly refused because `maintenance` is absent" from "mktemp failed
    for some other reason" -- assert the actual cause text too.
    """
    res = _call_flowd_guard(
        tmp_path, "0", "0", ["ALP-HE"], "fake-uart", None, setools_has_maintenance=False
    )
    assert res.returncode == 5, res.stderr
    assert "could not read the resident ATOC" in res.stderr, (
        f"exit 5 for the wrong reason -- expected the unverified-query abort, "
        f"got:\n{res.stderr}"
    )
    assert "SETOOLS 'maintenance' tool not found" in res.stderr, (
        f"the transcript must record why the query was unverified "
        f"(no maintenance binary), got:\n{res.stderr}"
    )


# --- alp-sdk#2187: the unverified-query abort must name the remedy that
# applies to the flow that reached it -------------------------------------
#
# bench_flowd_atoc_guard routes to the real query whenever $SE_UART is
# exported -- whether or not the variable names a device that can answer. A
# STALE or WRONG SE_UART therefore took the query path, the query failed, and
# the shared guard's abort recommended `--replace-atoc`: the "I checked and
# still want to replace it" override, for a check that demonstrably never
# ran. `unset SE_UART` -- the actual fix on a flow that needs no serial
# device -- was named nowhere.
#
# The refusal itself was always correct and is unchanged (exit 5, nothing
# written). What these tests pin is the TEXT, because the text is the defect:
# #2029/#2027 kept `--atoc-unqueryable` and `--replace-atoc` distinct
# precisely so a no-check state never trains an operator onto the
# checked-and-override flag, and flash-jlink.sh's own header says that habit
# must never form. Steering them onto it from the other direction re-conflates
# the two flags.
#
# Wording assertions rot, so each one below is anchored on the SMALLEST
# phrase that carries the actual behavioural claim -- the flag names and
# `unset SE_UART` -- never on a whole sentence.

#: Flow A's remedy, verbatim. On Flow A $SE_UART IS the transport, so a failed
#: query genuinely leaves confirming by hand and overriding as the only way
#: past -- this must keep saying exactly that.
_FLOW_A_REMEDY = "Confirm by hand what is resident, then re-run with --replace-atoc."


@_NEEDS_BASH
def test_flowd_unverified_query_does_not_recommend_replace_atoc(tmp_path):
    """The #2187 defect itself: SE_UART exported but unusable on Flow D.

    Still exit 5, still nothing written -- but the remedy must point at
    fixing or unsetting SE_UART, and must say in as many words that
    `--replace-atoc` is NOT the way out of a check that never ran.
    """
    res = _call_flowd_guard(
        tmp_path, "0", "0", ["ALP-HE"], "stale-uart", None, setools_has_maintenance=False
    )
    assert res.returncode == 5, res.stderr
    assert "could not read the resident ATOC" in res.stderr, res.stderr
    assert "unset SE_UART" in res.stderr, (
        f"Flow D's remedy must name unsetting SE_UART -- the flow needs no "
        f"serial device at all. Got:\n{res.stderr}"
    )
    assert "--atoc-unqueryable" in res.stderr, (
        f"Flow D's remedy must name the acknowledgement flag that fits a "
        f"check which did not run. Got:\n{res.stderr}"
    )
    assert "LG_PLACE" in res.stderr, (
        f"a wrong SE_UART is usually a stale raw path; the remedy must point "
        f"at the per-slot resolution instead. Got:\n{res.stderr}"
    )
    assert _FLOW_A_REMEDY not in res.stderr, (
        f"this is the #2187 defect: Flow D was told to re-run with "
        f"--replace-atoc, the checked-and-override flag, for a check that "
        f"never ran. Got:\n{res.stderr}"
    )


@_NEEDS_BASH
def test_flow_a_unverified_query_still_recommends_replace_atoc(tmp_path):
    """The other half, and the one a careless fix breaks: Flow A's wording is
    unchanged.

    On Flow A the SE-UART *is* the write transport, so there is no "unset it"
    remedy -- a human confirming what is resident and overriding is the only
    way past. A fix that made the new Flow D text unconditional would leave
    this green only if it were asserted, so assert it.
    """
    res = _call_atoc_guard(
        tmp_path, "0", ["ALP-HE"], "fake-uart", None, setools_has_maintenance=False
    )
    assert res.returncode == 5, res.stderr
    assert _FLOW_A_REMEDY in res.stderr, (
        f"Flow A's remedy must be unchanged by #2187. Got:\n{res.stderr}"
    )
    assert "unset SE_UART" not in res.stderr, (
        f"Flow A cannot write without its SE-UART, so unsetting it is never "
        f"the remedy there. Got:\n{res.stderr}"
    )


def _call_flowd_then_flow_a(tmp_path: Path) -> subprocess.CompletedProcess[str]:
    """Drive bench_flowd_atoc_guard and then bench_atoc_replace_guard in ONE
    shell, both landing on the unverified-query abort, and capture each
    call's stderr separately.

    This is the test the implementation shape exists for. The flow is
    selected by `BENCH_ATOC_FLOW`, which bench_flowd_atoc_guard sets with
    `local` so it is restored when that call returns. `local` inside a
    function is the whole guarantee: drop it (or hoist the assignment to the
    file scope) and the variable leaks, so every LATER Flow A guard in the
    same shell -- flash-run.sh and flash-run-dualcore.sh both call one --
    starts printing Flow D's "unset SE_UART" advice on a board whose SE-UART
    is the only way to write it. Nothing else in the tree would notice.
    """
    workdir = tmp_path
    (workdir / "bench-env.sh").write_bytes(ENV.read_bytes())

    # No `maintenance` binary at all: both guards take the unverified-query
    # abort, which is the branch whose wording is under test. Keeping both
    # calls on the same cause isolates the flow selector as the only thing
    # that can differ between the two transcripts.
    setools_dir = workdir / "setools-empty"
    setools_dir.mkdir(exist_ok=True)

    gate = workdir / "both-gates.sh"
    gate.write_bytes(
        (
            "unset LG_PLACE LG_COORDINATOR LG_SWD_PATH ALP_JLINK_SEARCH_ROOT\n"
            f'export SETOOLS_DIR="{setools_dir.name}"\n'
            'export SE_UART="stale-uart"\n'
            "source ./bench-env.sh\n"
            "bench_flowd_atoc_guard 0 0 flowd-first ALP-HE 2>flowd.err\n"
            "echo \"flowd=$?\"\n"
            "bench_atoc_replace_guard 0 flow-a-second ALP-HE 2>flowa.err\n"
            "echo \"flowa=$?\"\n"
            "exit 0\n"
        ).encode("utf-8")
    )
    return subprocess.run(
        ["bash", "both-gates.sh"], cwd=workdir, env=_sanitized_env(),
        capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60,
    )


@_NEEDS_BASH
def test_flowd_flow_selector_does_not_leak_into_a_later_flow_a_guard(tmp_path):
    """A Flow D guard must not change what a Flow A guard prints afterwards
    in the same shell."""
    res = _call_flowd_then_flow_a(tmp_path)
    flowd_err = (tmp_path / "flowd.err").read_text(encoding="utf-8")
    flowa_err = (tmp_path / "flowa.err").read_text(encoding="utf-8")

    assert "flowd=5" in res.stdout, f"the Flow D guard did not refuse: {res.stdout}"
    assert "flowa=5" in res.stdout, f"the Flow A guard did not refuse: {res.stdout}"

    assert "unset SE_UART" in flowd_err, (
        f"the first call must print Flow D's remedy. Got:\n{flowd_err}"
    )
    assert _FLOW_A_REMEDY in flowa_err, (
        f"the SECOND call is a Flow A guard and must print Flow A's remedy -- "
        f"BENCH_ATOC_FLOW leaked out of the Flow D call. Got:\n{flowa_err}"
    )
    assert "unset SE_UART" not in flowa_err, (
        f"Flow D's advice leaked into a Flow A guard: on a Flow A board the "
        f"SE-UART is the write transport and unsetting it is never the "
        f"remedy. Got:\n{flowa_err}"
    )



# The exact call bench_flowd_atoc_guard is invoked with in all three Flow D
# writers -- anchored to the literal argument shape, not merely the function
# name. Review MAJOR 1: a bare `"bench_flowd_atoc_guard" in body` substring
# check SURVIVES deleting the actual call from a script, because each
# script's own header comment (added by this same change) also names the
# function in prose. `grep -v '^bench_flowd_atoc_guard '` strips the real
# call and left the old assertion green -- a later refactor or bad merge
# could drop the call entirely with CI staying green, and the next
# flash-jlink-mramxip.sh run would silently delist a resident A32 Linux boot
# chain exactly as on 2026-09-07. Anchor to the call line itself.
_FLOWD_GUARD_CALL_RE = re.compile(
    r'^bench_flowd_atoc_guard "\$REPLACE_ATOC" "\$ATOC_UNQUERYABLE" '
)

# A REAL J-Link touch: either an actual `loadbin` command (a CommanderScript
# body line written into a heredoc, not a comment that merely mentions the
# word) or the JLinkExe invocation itself (every JLinkExe call in these three
# scripts carries `-CommanderScript`). Comments are excluded because this
# change's own new header comments say "loadbin" and "JLinkExe" in prose --
# e.g. flash-jlink.sh:36's "`loadbin` REPLACES it" -- well before the guard
# call, and a bare substring match would misdate the "first touch" to a hint
# line instead of a real invocation, making the ordering assertion below
# vacuous or wrong in the wrong direction.
_JLINK_TOUCH_RE = re.compile(r'(?:^|[^A-Za-z0-9_])loadbin[^A-Za-z0-9_]|-CommanderScript\b')


def _first_jlink_touch_line(body: str) -> int | None:
    """1-based line number of a script's first real J-Link touch, or None."""
    for i, line in enumerate(body.splitlines(), start=1):
        if line.lstrip().startswith("#"):
            continue
        if _JLINK_TOUCH_RE.search(line):
            return i
    return None


def test_every_flowd_atoc_writer_calls_the_flowd_guard() -> None:
    """The three SE-UART-less-by-design Flow D writers (alp-sdk#2027) must
    route through bench_flowd_atoc_guard specifically -- not a hand-rolled
    copy of its SE_UART-gated decision, not a bare bench_atoc_replace_guard
    call (which would make SE_UART a hard requirement of Flow D, the one
    thing #2027 exists to avoid), and not merely a comment that NAMES the
    function while the actual call has rotted away.

    Two things are pinned, both load-bearing:
      1. the LITERAL call line exists (anchored on its exact argument
         shape) -- a bare substring check on the function name alone does
         NOT prove the call still exists, see `_FLOWD_GUARD_CALL_RE` above;
      2. that call line precedes this script's own first real J-Link touch
         -- the whole point of alp-sdk#2027 is that the check runs before
         ANY write, not merely that it runs somewhere in the file.
    """
    for script in ("flash-jlink.sh", "flash-jlink-hp.sh", "flash-jlink-mramxip.sh"):
        body = (BENCH / script).read_text(encoding="utf-8")
        lines = body.splitlines()
        guard_line = next(
            (
                i for i, line in enumerate(lines, start=1)
                if _FLOWD_GUARD_CALL_RE.match(line.strip())
            ),
            None,
        )
        assert guard_line is not None, (
            f'{script} does not call bench_flowd_atoc_guard with its documented '
            f'arguments ("$REPLACE_ATOC" "$ATOC_UNQUERYABLE" ...) -- a bare '
            f"mention of the function name (e.g. in a comment) does not count"
        )
        touch_line = _first_jlink_touch_line(body)
        assert touch_line is not None, (
            f"{script}: could not locate a loadbin/-CommanderScript line to "
            f"compare the guard's position against"
        )
        assert guard_line < touch_line, (
            f"{script}: bench_flowd_atoc_guard call at line {guard_line} does "
            f"not precede the first J-Link touch at line {touch_line}"
        )
