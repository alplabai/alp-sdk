# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#2076 -- ram-run.sh must load+go and read back in SEPARATE JLinkExe
sessions, with no in-session halt between `go` and `mem8`.

Bench-measured on e1m-aen-evk-02/-03 (2026-09-13): an in-session second
`halt` issued after `go` + `Sleep` returned an incoherent core and the `mem8`
that followed it failed outright, on an app a separate, read-only attach
proved was still running cleanly. This file drives the REAL ram-run.sh
end-to-end against a fake `JLinkExe` and fake `arm-zephyr-eabi-{nm,readelf}`
on PATH, and inspects the CommanderScript files it actually generates -- the
same discipline as test_bench_jlink_connect_guard.py's read-back derivation,
just for the two-session split instead of the connect guard.
"""

from __future__ import annotations

import os
import stat
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
RAM_RUN = REPO / "scripts" / "bench" / "aen" / "ram-run.sh"


def _bash_can_run_a_script() -> bool:
    """Same discipline as test_bench_jlink_connect_guard.py's helper of the
    same name: presence of `bash` on PATH is not enough (Windows CI resolves
    the WSL launcher with no distribution installed). Probe by RUNNING it."""
    try:
        probe = subprocess.run(
            ["bash", "-c", "printf ok"],
            capture_output=True, text=True, timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return probe.returncode == 0 and probe.stdout.strip() == "ok"


_NEEDS_BASH = pytest.mark.skipif(
    not _bash_can_run_a_script(),
    reason="no working `bash` on this host",
)

# A fake mem8 dump line whose bytes ASCII-decode to "hi" -- proves the
# existing decoder still runs unchanged against session 2's transcript.
_FAKE_MEM8_LINE = "20000D00 = 68 69 00 00"

_FAKE_READELF = """\
#!/usr/bin/env bash
if [ "$1" = "-h" ]; then
	cat <<'HDR'
ELF Header:
  Entry point address:               0x0000001d
HDR
elif [ "$1" = "-l" ]; then
	cat <<'PHDR'
Elf file type is EXEC (Executable file)
Entry point 0x1d

Program Headers:
  Type           Offset   VirtAddr   PhysAddr   FileSiz MemSiz  Flg Align
  LOAD           0x001000 0x00000000 0x00000000 0x000100 0x000100 R E 0x1000
PHDR
fi
"""

_FAKE_NM = """\
#!/usr/bin/env bash
echo "20000d00 D ram_console_buf"
"""

# The fake JLinkExe: records every -CommanderScript file it is handed (in
# call order, under CALL_LOG_DIR) and, unless this is the call number named
# by FAIL_CALL, echoes each line back with a J-Link> prompt (so
# bench_jlink_assert_connected's prompt-presence check passes) plus a DPIDR
# line (for the preflight's bench_jlink_assert_aen_dpidr) and a canned mem8
# dump line whenever the script contains one.
_FAKE_JLINKEXE = f"""\
#!/usr/bin/env bash
script=""
prev=""
for a in "$@"; do
	[ "$prev" = "-CommanderScript" ] && script="$a"
	prev="$a"
done

n=0
[ -f "$CALL_LOG_DIR/n" ] && n=$(cat "$CALL_LOG_DIR/n")
n=$((n + 1))
echo "$n" > "$CALL_LOG_DIR/n"
cp "$script" "$CALL_LOG_DIR/call-$n.jlink"

echo "SEGGER J-Link Commander V9.50 (fake)"

if [ "$n" = "${{FAIL_CALL:-0}}" ]; then
	echo "J-Link>connect"
	echo "Connecting to J-Link ...FAILED: Cannot connect to the probe/programmer."
	echo "J-Link>exit"
	echo "Script processing completed."
	exit 0
fi

echo "Found SW-DP with ID 0x4C013477"
while IFS= read -r line; do
	echo "J-Link>$line"
	case "$line" in
	mem8*) echo "{_FAKE_MEM8_LINE}" ;;
	esac
done <"$script"
echo "Script processing completed."
"""


def _write_exe(path: Path, body: str) -> None:
    path.write_text(body, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)


def _run_ram_run(tmp_path: Path, fail_call: str | None = None) -> tuple[subprocess.CompletedProcess[str], Path]:
    """Run the real ram-run.sh against a fake JLinkExe + fake toolchain.

    Returns (completed process, call-log directory) so a test can inspect
    the actual generated CommanderScript files.
    """
    toolsdir = tmp_path / "tools"
    toolsdir.mkdir()
    _write_exe(toolsdir / "JLinkExe", _FAKE_JLINKEXE)
    _write_exe(toolsdir / "arm-zephyr-eabi-readelf", _FAKE_READELF)
    _write_exe(toolsdir / "arm-zephyr-eabi-nm", _FAKE_NM)

    calls = tmp_path / "calls"
    calls.mkdir()

    bd = tmp_path / "build"
    (bd / "zephyr").mkdir(parents=True)
    (bd / "zephyr" / "zephyr.elf").write_bytes(b"\x7fELF-fake")
    (bd / "zephyr" / "zephyr.bin").write_bytes(b"\x00" * 16)

    env = dict(os.environ)
    env["PATH"] = f"{toolsdir}{os.pathsep}{env.get('PATH', '')}"
    env["CALL_LOG_DIR"] = str(calls)
    env["ZEPHYR_SDK_INSTALL_DIR"] = ""
    env["JLINK_SN"] = ""
    # bench_jlink_exe() (bench-env.sh) prefers a real, versioned SEGGER
    # install glob under ALP_JLINK_SEARCH_ROOT/$HOME over a bare PATH lookup
    # -- on a bench-provisioned host that would silently pick the REAL
    # JLinkExe over this fake one. JLINK_EXE pins it explicitly, which
    # bench_jlink_exe() honours first.
    env["JLINK_EXE"] = str(toolsdir / "JLinkExe")
    if fail_call is not None:
        env["FAIL_CALL"] = fail_call
    else:
        env.pop("FAIL_CALL", None)

    res = subprocess.run(
        ["bash", str(RAM_RUN), str(bd), "10", "0x10"],
        capture_output=True, text=True, timeout=60, env=env,
    )
    return res, calls


@_NEEDS_BASH
def test_loadbin_go_and_mem8_are_in_separate_sessions(tmp_path: Path) -> None:
    res, calls = _run_ram_run(tmp_path)
    assert res.returncode == 0, f"expected success:\n{res.stdout}\n{res.stderr}"

    call_files = sorted(calls.glob("call-*.jlink"), key=lambda p: int(p.stem.split("-")[1]))
    assert len(call_files) == 3, (
        f"expected 3 JLinkExe sessions (preflight, load+go, read), got "
        f"{len(call_files)}: {[p.name for p in call_files]}"
    )
    preflight, load_go, read_back = call_files
    # Compare whole COMMAND LINES, not raw substrings -- the fixture's own
    # tmp_path (derived from this test's name) contains the literal
    # substring "mem8", so a bare `"mem8" in body` false-positives on the
    # `loadbin <path-containing-mem8> ...` line in the load+go session.
    load_lines = [ln.strip() for ln in load_go.read_text().splitlines()]
    read_lines = [ln.strip() for ln in read_back.read_text().splitlines()]
    preflight_body = preflight.read_text()

    assert any(ln.startswith("loadbin ") for ln in load_lines), load_lines
    assert "go" in load_lines, load_lines
    assert not any(ln.startswith("mem8") for ln in load_lines), (
        f"mem8 leaked into the load+go session: {load_lines}"
    )
    assert any(ln.startswith("mem8 ") for ln in read_lines), read_lines
    assert not any(ln.startswith("loadbin ") for ln in read_lines), (
        f"loadbin leaked into the read session: {read_lines}"
    )

    # The key assertion: no `halt` anywhere in the session that ran `go`,
    # and none in the read session either -- a halt in EITHER of those two
    # reintroduces the measured-broken in-session halt-then-read shape.
    assert "halt" not in load_lines[load_lines.index("go") :], (
        f"'halt' present at/after 'go' in the load+go session -- this is "
        f"the exact shape measured broken on evk-02/-03 (alp-sdk#2076): {load_lines}"
    )
    assert "halt" not in read_lines, (
        f"'halt' present in the read session -- reintroduces a halt "
        f"immediately before mem8, just moved to session 2: {read_lines}"
    )
    assert "AEN_DPIDR" not in preflight_body  # sanity: preflight is untouched by this change

    # The decoder must still work unchanged against session 2's transcript.
    assert "hi" in res.stdout
    assert "RAM console (decoded)" in res.stdout


@_NEEDS_BASH
def test_session_two_connect_failure_is_a_hard_error_not_an_empty_console(tmp_path: Path) -> None:
    """alp-sdk#1318 in the new two-session shape: a session-2 connect failure
    must fail the script with the infrastructure exit code (7), never fall
    through to print an empty 'RAM console (decoded)' block that reads as a
    crashed app."""
    res, calls = _run_ram_run(tmp_path, fail_call="3")

    assert res.returncode == 7, f"expected exit 7:\n{res.stdout}\n{res.stderr}"
    assert "RAM console (decoded)" not in res.stdout, (
        "printed the console header despite a failed session-2 connect -- "
        "this decodes as an empty (crashed-looking) console for an "
        "infrastructure reason, exactly the alp-sdk#1318 defect"
    )
    assert "could NOT connect" in res.stderr


@_NEEDS_BASH
def test_session_one_connect_failure_is_also_a_hard_error(tmp_path: Path) -> None:
    """Same guard, session 1: a load+go that never reached the probe must
    not proceed to sleep and attempt a session-2 read at all."""
    res, calls = _run_ram_run(tmp_path, fail_call="2")

    assert res.returncode == 7, f"expected exit 7:\n{res.stdout}\n{res.stderr}"
    call_files = list(calls.glob("call-*.jlink"))
    assert len(call_files) == 2, (
        "session 2 must never run after a session-1 connect failure, got "
        f"{len(call_files)} calls"
    )
