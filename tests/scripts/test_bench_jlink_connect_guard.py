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
    an AEN image on the V2N-M1 -- a different labgrid place."""
    res = _call_dpidr(tmp_path / "pf.out", _GD32_HIT + "\n")
    assert res.returncode == 4
    assert "GD32" in res.stderr
    assert "e1mx-v2n-m1-01" in res.stderr, "must name the place the operator does not hold"


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
