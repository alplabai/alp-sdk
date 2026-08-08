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


def test_successful_read_passes(tmp_path: Path) -> None:
    out = tmp_path / "jlink.out"
    out.write_text(REAL_GOOD_READ, encoding="utf-8")

    res = _call_guard(out)

    assert res.returncode == 0, f"guard must not fire on a good read:\n{res.stderr}"


def test_empty_output_is_a_hard_error(tmp_path: Path) -> None:
    """A missing/empty transcript is also a failure -- decoding it yields the
    same empty block, so it must not pass silently."""
    out = tmp_path / "jlink.out"
    out.write_text("", encoding="utf-8")

    res = _call_guard(out)

    assert res.returncode == 7
    assert "no J-Link output at all" in res.stderr


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

    # Accept either the shared constants from bench-env.sh (preferred -- it is
    # the single source for both IDs) or a literal, but require BOTH sides of
    # the gate: confirm the AEN E8 and explicitly reject the V2N-M1 GD32.
    assert "AEN_DPIDR" in body or "4C013477" in body, (
        f"{script} writes MRAM over J-Link but never confirms the AEN E8 SW-DP IDR"
    )
    assert "GD32_DPIDR" in body or "0BE12477" in body, (
        f"{script} must explicitly reject the V2N-M1 GD32 SW-DP IDR"
    )
    # The gate has to abort, not warn.
    assert "ABORT" in body, f"{script}'s DPIDR check must hard-abort, not warn"
