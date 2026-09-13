# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#2076 -- ram-run.sh must load+go and read back in SEPARATE JLinkExe
sessions, routed through the board-farm's per-probe isolation wrapper, with
no in-session halt/reset between `go` and `mem8`, and no silent decode of a
failed load or a failed read.

Bench-measured on e1m-aen-evk-02/-03 (2026-09-13): an in-session second
`halt` issued after `go` + `Sleep` returned an incoherent core and the `mem8`
that followed it failed outright, on an app a separate, read-only attach
proved was still running cleanly. A later bench run (examples/peripheral-io/
blink, 6 of 6 clean, both boards) confirmed a plain `exit` after `go` leaves
the core running.

This file drives the REAL ram-run.sh end-to-end against a fake
`AEN_JLINK_RUN` wrapper (sharing jlink-run.sh's own "<place> [JLinkExe
args...]" contract, per review) and fake `arm-zephyr-eabi-{nm,readelf}` on
PATH, and inspects the CommandFile files it actually generates -- the same
discipline as test_bench_jlink_connect_guard.py's read-back derivation, just
for the two-session split instead of the connect guard.
"""

from __future__ import annotations

import os
import stat
import subprocess
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

# The fake AEN_JLINK_RUN wrapper: shares jlink-run.sh's own CLI shape
# (`<place> [JLinkExe args...]`) and its `-CommandFile <path>` flag (NOT
# `-CommanderScript` -- ram-run.sh must use the flag the real wrapper
# recognizes so it can inject its firmware-update guard ahead of the
# script). Records every call (in order, under CALL_LOG_DIR): the place
# name, a wall-clock timestamp (for the inter-session sleep test), and a
# copy of the CommandFile content -- then echoes each command back with a
# J-Link> prompt (bench_jlink_assert_connected's prompt check) plus a DPIDR
# line (bench_jlink_assert_aen_dpidr) and a canned reply for `loadbin`/
# `mem8`, controllable per test via FAIL_CALL / FAIL_LOADBIN / FAIL_READ_NOMEM
# / FAIL_READ_NODUMP.
_FAKE_JLINK_RUN = f"""\
#!/usr/bin/env bash
place="$1"; shift
script=""
prev=""
for a in "$@"; do
	[ "$prev" = "-CommandFile" ] && script="$a"
	prev="$a"
done

n=0
[ -f "$CALL_LOG_DIR/n" ] && n=$(cat "$CALL_LOG_DIR/n")
n=$((n + 1))
echo "$n" > "$CALL_LOG_DIR/n"
echo "$place" > "$CALL_LOG_DIR/place-$n"
date +%s.%N > "$CALL_LOG_DIR/ts-$n"
cp "$script" "$CALL_LOG_DIR/call-$n.jlink"

echo "jlink-run: place=$place port=fake (fake wrapper)"
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
	loadbin*)
		if [ -n "${{FAIL_LOADBIN:-}}" ]; then
			echo "Writing target memory failed."
		else
			echo "O.K."
		fi
		;;
	mem8*)
		if [ -n "${{FAIL_READ_NOMEM:-}}" ]; then
			echo "Could not read memory."
		elif [ -n "${{FAIL_READ_NODUMP:-}}" ]; then
			:
		else
			echo "{_FAKE_MEM8_LINE}"
		fi
		;;
	esac
done <"$script"
echo "Script processing completed."
"""


def _write_exe(path: Path, body: str) -> None:
    path.write_text(body, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)


def _run_ram_run(
    tmp_path: Path,
    sleep_ms: str = "10",
    fail_call: str | None = None,
    bench_place: str | None = "test-place",
    set_aen_jlink_run: bool = True,
    extra_env: dict[str, str] | None = None,
) -> tuple[subprocess.CompletedProcess[str], Path]:
    """Run the real ram-run.sh against a fake AEN_JLINK_RUN wrapper + fake
    toolchain.

    Returns (completed process, call-log directory) so a test can inspect
    the actual generated CommandFile files and per-call timestamps.
    """
    toolsdir = tmp_path / "tools"
    toolsdir.mkdir()
    _write_exe(toolsdir / "arm-zephyr-eabi-readelf", _FAKE_READELF)
    _write_exe(toolsdir / "arm-zephyr-eabi-nm", _FAKE_NM)

    wrapper = tmp_path / "jlink-run.sh"
    _write_exe(wrapper, _FAKE_JLINK_RUN)

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
    env.pop("JLINK_SN", None)
    env.pop("JLINK_EXE", None)
    if bench_place is None:
        env.pop("BENCH_PLACE", None)
    else:
        env["BENCH_PLACE"] = bench_place
    if set_aen_jlink_run:
        env["AEN_JLINK_RUN"] = str(wrapper)
    else:
        env.pop("AEN_JLINK_RUN", None)
    if fail_call is not None:
        env["FAIL_CALL"] = fail_call
    else:
        env.pop("FAIL_CALL", None)
    if extra_env:
        env.update(extra_env)

    res = subprocess.run(
        ["bash", str(RAM_RUN), str(bd), sleep_ms, "0x10"],
        capture_output=True, text=True, timeout=60, env=env,
    )
    return res, calls


# JLinkExe commands whose PRESENCE re-halts/resets the core -- forbidden
# from `go` onward in the load session and ANYWHERE in the read session.
# `h`/`r` are JLinkExe's own one-letter aliases for halt/reset, easy to miss
# with a bare `"halt" not in lines` check (review finding 7).
_HALT_OR_RESET_TOKENS = {"h", "halt", "r", "reset"}


def _assert_no_halt_or_reset(lines: list[str], where: str) -> None:
    for ln in lines:
        token = ln.split()[0] if ln.split() else ""
        assert token not in _HALT_OR_RESET_TOKENS, (
            f"'{ln}' re-halts/resets the core in {where} -- this is the exact "
            f"shape measured broken on evk-02/-03 (alp-sdk#2076): {lines}"
        )
        assert not ln.startswith("RSetType"), (
            f"'{ln}' issues a pin reset in {where}: {lines}"
        )


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

    # The key assertion: no halt/reset command anywhere from `go` onward in
    # the load session, and none anywhere in the read session -- covers the
    # one-letter `h`/`r` aliases and `RSetType`, not just the literal word
    # "halt" (review finding 7).
    _assert_no_halt_or_reset(load_lines[load_lines.index("go"):], "the load+go session")
    _assert_no_halt_or_reset(read_lines, "the read session")

    # The preflight stays a minimal, read-only connect check -- it must
    # never itself carry the load or the read.
    assert "loadbin" not in preflight_body and "mem8" not in preflight_body

    # BENCH_PLACE actually reached the wrapper -- the whole point of routing
    # through it (review finding 2).
    for i in (1, 2, 3):
        assert (calls / f"place-{i}").read_text().strip() == "test-place"

    # The decoder must still work unchanged against session 2's transcript.
    assert "hi" in res.stdout
    assert "RAM console (decoded)" in res.stdout


@_NEEDS_BASH
def test_a_real_time_gap_separates_the_two_sessions(tmp_path: Path) -> None:
    """The host must actually sleep between sessions -- a deleted `sleep`
    call (review finding 7) would leave call 2 and call 3 back to back."""
    res, calls = _run_ram_run(tmp_path, sleep_ms="500")
    assert res.returncode == 0, f"expected success:\n{res.stdout}\n{res.stderr}"

    ts2 = float((calls / "ts-2").read_text().strip())
    ts3 = float((calls / "ts-3").read_text().strip())
    gap = ts3 - ts2
    assert gap >= 0.4, (
        f"only {gap:.3f}s between the load+go session and the read session "
        f"for sleep_ms=500 -- the host-side sleep looks deleted or too short"
    )


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


@_NEEDS_BASH
def test_session_two_could_not_read_memory_is_a_hard_error(tmp_path: Path) -> None:
    """Review finding 1: a CONNECTED session-2 whose `mem8` itself reports
    'Could not read memory.' must not decode as an (empty) console -- this
    is exactly the silent failure mode #2076 is about, and with no root
    cause established a no-halt read can still fail this way."""
    res, _ = _run_ram_run(tmp_path, extra_env={"FAIL_READ_NOMEM": "1"})

    assert res.returncode not in (0, 7), (
        f"expected a non-zero, non-connect-guard exit for a read that "
        f"itself failed:\n{res.stdout}\n{res.stderr}"
    )
    assert "RAM console (decoded)" not in res.stdout
    assert "Could not read memory" in res.stderr


@_NEEDS_BASH
def test_session_two_no_dump_line_is_a_hard_error(tmp_path: Path) -> None:
    """Same finding, the quieter half: a session-2 transcript with no
    'ADDR = HH HH ...' dump line at all (mem8 silently produced nothing)
    must also refuse to decode, not just the explicit-error case."""
    res, _ = _run_ram_run(tmp_path, extra_env={"FAIL_READ_NODUMP": "1"})

    assert res.returncode not in (0, 7)
    assert "RAM console (decoded)" not in res.stdout


@_NEEDS_BASH
def test_session_one_loadbin_failure_is_a_hard_error(tmp_path: Path) -> None:
    """Review finding 4: a CONNECTED session-1 whose `loadbin` itself fails
    must not proceed to sleep + read -- DTCM survives the reset `loadbin`
    triggers, so a stale image from an earlier run can boot and its console
    would be read back as if THIS load had succeeded."""
    res, calls = _run_ram_run(tmp_path, extra_env={"FAIL_LOADBIN": "1"})

    assert res.returncode not in (0, 7), f"{res.stdout}\n{res.stderr}"
    call_files = list(calls.glob("call-*.jlink"))
    assert len(call_files) == 2, (
        "session 2 must never run after a failed loadbin, got "
        f"{len(call_files)} calls"
    )
    assert "RAM console (decoded)" not in res.stdout


@_NEEDS_BASH
def test_missing_bench_place_refuses_before_any_probe_access(tmp_path: Path) -> None:
    res, calls = _run_ram_run(tmp_path, bench_place=None)

    assert res.returncode == 2, f"expected exit 2:\n{res.stdout}\n{res.stderr}"
    assert "BENCH_PLACE" in res.stderr
    assert not list(calls.glob("call-*.jlink")), "no probe access without BENCH_PLACE"


@_NEEDS_BASH
@pytest.mark.parametrize("bad_sleep", ["0x10", "abc", "-5", "1.5"])
def test_invalid_sleep_ms_refuses_before_any_probe_access(tmp_path: Path, bad_sleep: str) -> None:
    res, calls = _run_ram_run(tmp_path, sleep_ms=bad_sleep)

    assert res.returncode == 2, f"expected exit 2 for sleep_ms={bad_sleep!r}:\n{res.stdout}\n{res.stderr}"
    assert not list(calls.glob("call-*.jlink")), (
        f"sleep_ms={bad_sleep!r} touched the probe before being rejected"
    )
