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
the core running, AND surfaced two review-round-2 bugs on real hardware: the
`loadbin ... O.K.` success check was too narrow (a real transcript puts six
lines between the echoed command and `O.K.`, not "immediately after") and,
separately, too loose (a whole-transcript case-insensitive scan matches the
OPERATOR'S OWN build-dir path). Both are fixed here with a window anchored
on the exact echoed command, scoped to the next `J-Link>` prompt, requiring
an exact (not substring) `O.K.` line.

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

# ram-run.sh's own header declares it Linux/WSL2-only bench tooling (it
# drives JLinkExe via a board-farm wrapper that assumes labgrid + a Linux
# USB/mount-namespace stack). This file is the first thing to actually
# EXECUTE the real script rather than just its bench-env.sh helpers, and
# doing so surfaces two macOS-only breaks that predate this fix and are out
# of scope to chase here: BSD `date` has no `%N` (worked around below with
# `python3 -c 'import time; ...'` instead) and macOS's awk lacks
# `strtonum()`, which ram-run.sh's own LOAD-segment parsing depends on
# (scripts/bench/aen/ram-run.sh) -- review round 3 (alp-sdk#2076).
_NEEDS_LINUX = pytest.mark.skipif(
    not sys.platform.startswith("linux"),
    reason="ram-run.sh is Linux/WSL2-only bench tooling (its own header); "
           "this drives the real script end-to-end, which needs a Linux awk",
)

def _needs_e2e(func):
    """Both guards: a working bash AND a Linux host (see _NEEDS_LINUX)."""
    return _NEEDS_BASH(_NEEDS_LINUX(func))


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

# Verbatim from a real JLinkExe V9.74 transcript through jlink-run.sh on
# e1m-aen-evk-02 (2026-09-13) -- SIX lines between the echoed `loadbin`
# command and `O.K.`, not "immediately after" (the bug a `grep -A3` window
# missed). Used as the fake wrapper's SUCCESSFUL loadbin reply so the
# ordinary happy-path tests below exercise the real shape, not a
# simplified one that would hide a regression back to the narrow check.
_REAL_LOADBIN_OK_BLOCK = """\
'loadbin': Performing implicit reset & halt of MCU.
Reset type: NORMAL (https://kb.segger.com/J-Link_Reset_Strategies)
Reset: ARMv8M core with Security Extension enabled detected. Switch to secure domain.
Reset: Halt core after reset via DEMCR.VC_CORERESET.
Reset: Reset device via AIRCR.SYSRESETREQ.
Downloading file [/some/path]...
O.K."""

# The fake AEN_JLINK_RUN wrapper: shares jlink-run.sh's own CLI shape
# (`<place> [JLinkExe args...]`) and its `-CommandFile <path>` flag (NOT
# `-CommanderScript` -- ram-run.sh must use the flag the real wrapper
# recognizes so it can inject its firmware-update guard ahead of the
# script). Records every call (in order, under CALL_LOG_DIR): the place
# name, a wall-clock timestamp (for the inter-session sleep test, via
# `python3 -c 'import time'` -- BSD `date` has no `%N`, review round 3), and
# a copy of the CommandFile content -- then echoes each command back with a
# J-Link> prompt (bench_jlink_assert_connected's prompt check) plus a DPIDR
# line (bench_jlink_assert_aen_dpidr) and a canned reply for `loadbin`/
# `mem8`, controllable per test via FAIL_CALL / FAIL_LOADBIN /
# FAIL_READ_NOMEM / FAIL_READ_NODUMP.
#
# `loadbin` handling replies with the REAL multi-line success block for
# every `loadbin` in the script EXCEPT THE LAST one, which fails (prints no
# `O.K.` at all) when FAIL_LOADBIN is set -- a preload file's own `loadbin`
# always precedes the script's main one in the generated CommandFile, so
# this lets one test prove the success/failure check is anchored on the
# SPECIFIC main-image command, not "any loadbin, anywhere".
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
python3 -c 'import time; print(time.time())' > "$CALL_LOG_DIR/ts-$n"
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
total_loadbins=$(grep -c '^loadbin' "$script")
idx=0
while IFS= read -r line; do
	echo "J-Link>$line"
	case "$line" in
	loadbin*)
		idx=$((idx + 1))
		if [ "$idx" = "$total_loadbins" ] && [ -n "${{FAIL_LOADBIN:-}}" ]; then
			echo "Writing target memory failed."
		else
			cat <<'LOADBIN_OK_EOF'
{_REAL_LOADBIN_OK_BLOCK}
LOADBIN_OK_EOF
		fi
		;;
	mem8*)
		if [ -n "${{FAIL_READ_NOMEM:-}}" ]; then
			echo "{_FAKE_MEM8_LINE}"
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


_UNSET = object()


def _run_ram_run(
    tmp_path: Path,
    sleep_ms: str = "10",
    fail_call: str | None = None,
    bench_place: str | None = "test-place",
    aen_jlink_run: object = _UNSET,
    bd_name: str = "build",
    preload: str | None = None,
    extra_env: dict[str, str] | None = None,
) -> tuple[subprocess.CompletedProcess[str], Path, Path]:
    """Run the real ram-run.sh against a fake AEN_JLINK_RUN wrapper + fake
    toolchain.

    `aen_jlink_run`, left at its default (`_UNSET`), points AEN_JLINK_RUN at
    the fake wrapper -- ALWAYS explicitly, never by omission: a forgotten
    override on a real bench host would silently drive a real probe, which
    is exactly why ram-run.sh itself has no default for this var either
    (review round 3, finding 6). Pass `None` to leave it unset (for the
    dedicated "AEN_JLINK_RUN missing" test) or a string to point it
    somewhere else entirely.

    Returns (completed process, call-log directory, sandboxed TMPDIR) so a
    test can inspect the generated CommandFile files, per-call timestamps,
    and whatever ram-run.sh's own WORKDIR left behind under TMPDIR.
    """
    toolsdir = tmp_path / "tools"
    toolsdir.mkdir(exist_ok=True)
    _write_exe(toolsdir / "arm-zephyr-eabi-readelf", _FAKE_READELF)
    _write_exe(toolsdir / "arm-zephyr-eabi-nm", _FAKE_NM)

    wrapper = tmp_path / "jlink-run.sh"
    if not wrapper.exists():
        _write_exe(wrapper, _FAKE_JLINK_RUN)

    calls = tmp_path / "calls"
    calls.mkdir(exist_ok=True)

    sandbox_tmpdir = tmp_path / "tmpdir"
    sandbox_tmpdir.mkdir(exist_ok=True)

    bd = tmp_path / bd_name
    (bd / "zephyr").mkdir(parents=True, exist_ok=True)
    (bd / "zephyr" / "zephyr.elf").write_bytes(b"\x7fELF-fake")
    (bd / "zephyr" / "zephyr.bin").write_bytes(b"\x00" * 16)

    env = dict(os.environ)
    env["PATH"] = f"{toolsdir}{os.pathsep}{env.get('PATH', '')}"
    env["CALL_LOG_DIR"] = str(calls)
    env["TMPDIR"] = str(sandbox_tmpdir)
    env["ZEPHYR_SDK_INSTALL_DIR"] = ""
    env.pop("JLINK_SN", None)
    env.pop("JLINK_EXE", None)
    if bench_place is None:
        env.pop("BENCH_PLACE", None)
    else:
        env["BENCH_PLACE"] = bench_place
    if aen_jlink_run is _UNSET:
        env["AEN_JLINK_RUN"] = str(wrapper)
    elif aen_jlink_run is None:
        env.pop("AEN_JLINK_RUN", None)
    else:
        env["AEN_JLINK_RUN"] = str(aen_jlink_run)
    if fail_call is not None:
        env["FAIL_CALL"] = fail_call
    else:
        env.pop("FAIL_CALL", None)
    if extra_env:
        env.update(extra_env)

    args = ["bash", str(RAM_RUN), str(bd), sleep_ms, "0x10"]
    if preload is not None:
        args.append(preload)

    res = subprocess.run(
        args, capture_output=True, text=True, timeout=60, env=env,
    )
    return res, calls, sandbox_tmpdir


def _workdirs(sandbox_tmpdir: Path) -> list[Path]:
    return sorted(sandbox_tmpdir.glob("ram-run.*"))


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


@_needs_e2e
def test_loadbin_go_and_mem8_are_in_separate_sessions(tmp_path: Path) -> None:
    res, calls, sandbox_tmpdir = _run_ram_run(tmp_path)
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

    # WORKDIR removed on a successful exit (review finding 11).
    assert _workdirs(sandbox_tmpdir) == [], (
        f"WORKDIR left behind after a successful run: {_workdirs(sandbox_tmpdir)}"
    )


@_needs_e2e
def test_a_real_time_gap_separates_the_two_sessions(tmp_path: Path) -> None:
    """The host must actually sleep between sessions -- a deleted `sleep`
    call (review finding 7) would leave call 2 and call 3 back to back."""
    res, calls, _ = _run_ram_run(tmp_path, sleep_ms="500")
    assert res.returncode == 0, f"expected success:\n{res.stdout}\n{res.stderr}"

    ts2 = float((calls / "ts-2").read_text().strip())
    ts3 = float((calls / "ts-3").read_text().strip())
    gap = ts3 - ts2
    assert gap >= 0.4, (
        f"only {gap:.3f}s between the load+go session and the read session "
        f"for sleep_ms=500 -- the host-side sleep looks deleted or too short"
    )


@_needs_e2e
def test_session_two_connect_failure_is_a_hard_error_not_an_empty_console(tmp_path: Path) -> None:
    """alp-sdk#1318 in the new two-session shape: a session-2 connect failure
    must fail the script with the infrastructure exit code (7), never fall
    through to print an empty 'RAM console (decoded)' block that reads as a
    crashed app."""
    res, calls, sandbox_tmpdir = _run_ram_run(tmp_path, fail_call="3")

    assert res.returncode == 7, f"expected exit 7:\n{res.stdout}\n{res.stderr}"
    assert "RAM console (decoded)" not in res.stdout, (
        "printed the console header despite a failed session-2 connect -- "
        "this decodes as an empty (crashed-looking) console for an "
        "infrastructure reason, exactly the alp-sdk#1318 defect"
    )
    assert "could NOT connect" in res.stderr
    # The stale bench-env.sh JLINK_SN hint is corrected, not left dangling
    # (review finding 5).
    assert "BENCH_PLACE" in res.stderr

    # A failing run KEEPS its WORKDIR (review finding 2/11) -- exactly one.
    left = _workdirs(sandbox_tmpdir)
    assert len(left) == 1, f"expected exactly one WORKDIR kept on failure, got {left}"


@_needs_e2e
def test_session_one_connect_failure_is_also_a_hard_error(tmp_path: Path) -> None:
    """Same guard, session 1: a load+go that never reached the probe must
    not proceed to sleep and attempt a session-2 read at all."""
    res, calls, _ = _run_ram_run(tmp_path, fail_call="2")

    assert res.returncode == 7, f"expected exit 7:\n{res.stdout}\n{res.stderr}"
    call_files = list(calls.glob("call-*.jlink"))
    assert len(call_files) == 2, (
        "session 2 must never run after a session-1 connect failure, got "
        f"{len(call_files)} calls"
    )


@_needs_e2e
def test_session_two_could_not_read_memory_is_a_hard_error(tmp_path: Path) -> None:
    """Review finding 1: a CONNECTED session-2 whose `mem8` itself reports
    'Could not read memory.' must not decode as an (empty) console -- this
    is exactly the silent failure mode #2076 is about, and with no root
    cause established a no-halt read can still fail this way. The fake here
    prints a DUMP LINE first (review finding 10) so this exercises the
    Could-not-read-memory check itself, not just the "no dump line" branch."""
    res, _, _ = _run_ram_run(tmp_path, extra_env={"FAIL_READ_NOMEM": "1"})

    assert res.returncode == 9, f"expected exit 9:\n{res.stdout}\n{res.stderr}"
    assert "RAM console (decoded)" not in res.stdout
    assert "Could not read memory" in res.stderr


@_needs_e2e
def test_session_two_no_dump_line_is_a_hard_error(tmp_path: Path) -> None:
    """Same finding, the quieter half: a session-2 transcript with no
    'ADDR = HH HH ...' dump line at all (mem8 silently produced nothing)
    must also refuse to decode, not just the explicit-error case."""
    res, _, _ = _run_ram_run(tmp_path, extra_env={"FAIL_READ_NODUMP": "1"})

    assert res.returncode == 9, f"expected exit 9:\n{res.stdout}\n{res.stderr}"
    assert "RAM console (decoded)" not in res.stdout


@_needs_e2e
def test_session_one_loadbin_failure_is_a_hard_error(tmp_path: Path) -> None:
    """Review finding 4/1: a CONNECTED session-1 whose `loadbin` itself
    fails must not proceed to sleep + read -- DTCM survives the reset
    `loadbin` triggers, so a stale image from an earlier run can boot and
    its console would be read back as if THIS load had succeeded."""
    res, calls, _ = _run_ram_run(tmp_path, extra_env={"FAIL_LOADBIN": "1"})

    assert res.returncode == 8, f"expected exit 8:\n{res.stdout}\n{res.stderr}"
    call_files = list(calls.glob("call-*.jlink"))
    assert len(call_files) == 2, (
        "session 2 must never run after a failed loadbin, got "
        f"{len(call_files)} calls"
    )
    assert "RAM console (decoded)" not in res.stdout


@_needs_e2e
def test_a_real_successful_loadbin_transcript_passes(tmp_path: Path) -> None:
    """Review finding 1, the too-narrow direction: bench-measured 2026-09-13
    that a real successful loadbin puts SIX lines between the echoed command
    and 'O.K.', not immediately after -- a `grep -A3` window exited 8 on 6 of
    6 clean bench runs. The fake wrapper's default loadbin reply IS that real
    block (see _REAL_LOADBIN_OK_BLOCK), so plain success (already exercised
    by test_loadbin_go_and_mem8_are_in_separate_sessions) already covers
    this; this test exists to name the regression explicitly."""
    res, _, _ = _run_ram_run(tmp_path)
    assert res.returncode == 0, (
        f"a real (6-line-then-O.K.) successful loadbin transcript must pass:\n"
        f"{res.stdout}\n{res.stderr}"
    )


@_needs_e2e
def test_a_failed_loadbin_with_ok_in_the_build_path_still_fails(tmp_path: Path) -> None:
    """Review finding 1, the too-loose direction: the reviewer's own
    reproduction -- a build directory literally named so its path contains
    the substring "O.K." case-insensitively ("demo.k.build" contains
    "o.k.") -- combined with a GENUINE loadbin failure ("Writing target
    memory failed.", no true O.K. reply) must still exit 8. A whole-window
    case-insensitive scan (the pre-fix shape) would find "O.K." inside the
    ECHOED COMMAND LINE itself (which embeds the operator's own path) and
    wrongly pass."""
    assert "o.k." in "demo.k.build".lower()
    res, calls, _ = _run_ram_run(
        tmp_path, bd_name="demo.k.build", extra_env={"FAIL_LOADBIN": "1"}
    )
    assert res.returncode == 8, (
        f"a build path containing 'O.K.' as a substring must not mask a "
        f"real loadbin failure:\n{res.stdout}\n{res.stderr}"
    )


@_needs_e2e
def test_a_preload_loadbin_success_does_not_mask_the_main_loadbin_failure(tmp_path: Path) -> None:
    """Review finding 1: a preload file's OWN successful `loadbin` (a
    legitimate use -- e.g. clearing a SoC integration register via a small
    load) must not satisfy the check for the SCRIPT's main image load. The
    fake always answers every `loadbin` but the LAST one with a real success
    block, and fails only the last -- since the preload's commands are
    spliced in before the main `loadbin $BIN $BASE` line in the generated
    CommandFile, the main one is always last."""
    preload_file = tmp_path / "preload.jlink"
    preload_file.write_text("loadbin /some/other/preload.bin 0x02000000\n", encoding="utf-8")

    res, calls, _ = _run_ram_run(
        tmp_path, preload=str(preload_file), extra_env={"FAIL_LOADBIN": "1"}
    )
    assert res.returncode == 8, (
        f"the preload's own successful loadbin masked the main image's "
        f"failure:\n{res.stdout}\n{res.stderr}"
    )
    # Count LINES starting with "loadbin ", not a substring scan -- the
    # tmp_path this test itself runs under can embed "loadbin" inside the
    # build directory name it derives from this test's own name, which a
    # bare substring count would double-count (the same class of
    # self-interference as the "mem8"-in-tmp_path trap elsewhere in this
    # file).
    load_lines = (sorted(calls.glob("call-*.jlink"))[1]).read_text().splitlines()
    n_loadbins = sum(1 for ln in load_lines if ln.strip().startswith("loadbin "))
    assert n_loadbins == 2, f"expected exactly 2 loadbin lines (preload + main): {load_lines}"


@_needs_e2e
def test_missing_bench_place_refuses_before_any_probe_access(tmp_path: Path) -> None:
    res, calls, _ = _run_ram_run(tmp_path, bench_place=None)

    assert res.returncode == 2, f"expected exit 2:\n{res.stdout}\n{res.stderr}"
    assert "BENCH_PLACE" in res.stderr
    assert not list(calls.glob("call-*.jlink")), "no probe access without BENCH_PLACE"


@_needs_e2e
def test_missing_aen_jlink_run_refuses_before_any_probe_access(tmp_path: Path) -> None:
    """Review finding 6: AEN_JLINK_RUN has NO default -- unlike a forgotten
    override that would fall back to bench_jlink_exe()'s real-install
    search on an actual bench host, this must refuse closed."""
    res, calls, _ = _run_ram_run(tmp_path, aen_jlink_run=None)

    assert res.returncode == 2, f"expected exit 2:\n{res.stdout}\n{res.stderr}"
    assert "AEN_JLINK_RUN" in res.stderr
    assert not list(calls.glob("call-*.jlink")), "no probe access without AEN_JLINK_RUN"


@_needs_e2e
@pytest.mark.parametrize("bad_sleep", ["0x10", "abc", "-5", "1.5"])
def test_invalid_sleep_ms_refuses_before_any_probe_access(tmp_path: Path, bad_sleep: str) -> None:
    res, calls, _ = _run_ram_run(tmp_path, sleep_ms=bad_sleep)

    assert res.returncode == 2, f"expected exit 2 for sleep_ms={bad_sleep!r}:\n{res.stdout}\n{res.stderr}"
    assert not list(calls.glob("call-*.jlink")), (
        f"sleep_ms={bad_sleep!r} touched the probe before being rejected"
    )
