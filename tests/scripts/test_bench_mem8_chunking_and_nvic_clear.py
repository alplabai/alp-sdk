# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#2313 -- chunk mem8 reads >0x10000 and clear NVIC before an
openocd-ram-run.sh HE resume.

Two independent AEN bench-script gaps:

1. ``scripts/bench/aen/ram-run.sh`` / ``reread.sh`` read the RAM console
   with a single JLinkExe ``mem8 <addr>,<size>``. JLinkExe caps ``NumBytes``
   at ``0x10000`` ("NumBytes should be <= 0x10000"), so any read bigger than
   that returns nothing at all -- silently. ``bench_mem8_chunks()``
   (bench-env.sh) splits any read into ``mem8`` lines of at most ``0x10000``
   bytes each; both scripts route through it.

2. ``scripts/bench/aen/openocd-ram-run.sh``'s HE (and shared HP) path zeroed
   MSPLIM/PSPLIM before resuming a freshly loaded image but left NVIC state
   untouched -- a resident app that left an IRQ enabled/pending ran the new
   image straight into that app's ISR (seen: IRQn 333 CDC_SCANLINE0, IPSR
   0x15D). The script now clears every NVIC ICER/ICPR before resuming.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BENCH = REPO / "scripts" / "bench" / "aen"
ENV = BENCH / "bench-env.sh"
RAM_RUN = BENCH / "ram-run.sh"
REREAD = BENCH / "reread.sh"
OPENOCD_RAM_RUN = BENCH / "openocd-ram-run.sh"


def _bash_can_run_a_script() -> bool:
    """Same discipline as the sibling bench test files: presence of `bash`
    on PATH is not enough (Windows CI resolves the WSL launcher with no
    distribution installed). Probe by RUNNING it."""
    try:
        probe = subprocess.run(
            ["bash", "-c", "printf ok"],
            capture_output=True, text=True, encoding="utf-8", timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return probe.returncode == 0 and probe.stdout.strip() == "ok"


_NEEDS_BASH = pytest.mark.skipif(
    not _bash_can_run_a_script(),
    reason="no working `bash` on this host",
)


def _call_chunks(tmp_path: Path, addr: str, size: str) -> subprocess.CompletedProcess[str]:
    """Source bench-env.sh (with no labgrid inputs, so it never reaches real
    infrastructure) and print bench_mem8_chunks' output for <addr, size>."""
    (tmp_path / "bench-env.sh").write_bytes(ENV.read_bytes())
    script = (
        "unset LG_PLACE LG_COORDINATOR LG_SWD_PATH ALP_JLINK_SEARCH_ROOT\n"
        "source ./bench-env.sh\n"
        f'bench_mem8_chunks "{addr}" "{size}"\n'
        "exit $?\n"
    )
    gate = tmp_path / "gate.sh"
    gate.write_text(script, encoding="utf-8")
    return subprocess.run(
        ["bash", "gate.sh"], cwd=tmp_path, capture_output=True,
        text=True, encoding="utf-8", errors="replace", timeout=60,
    )


@_NEEDS_BASH
def test_chunk_size_below_cap_is_one_line(tmp_path: Path) -> None:
    """A read within JLinkExe's own 0x10000 limit stays a single mem8 line
    -- no behaviour change for the common case."""
    res = _call_chunks(tmp_path, "0x20000d00", "0x600")
    assert res.returncode == 0, res.stderr
    assert res.stdout.strip() == "mem8 0x20000D00, 0x600"


@_NEEDS_BASH
def test_chunk_size_exactly_at_cap_is_one_line(tmp_path: Path) -> None:
    res = _call_chunks(tmp_path, "0x20000d00", "0x10000")
    assert res.returncode == 0, res.stderr
    assert res.stdout.strip() == "mem8 0x20000D00, 0x10000"


@_NEEDS_BASH
def test_chunk_size_above_cap_splits_ceil_size_over_0x10000(tmp_path: Path) -> None:
    """The issue's own worked example: size 0x14000 gives 0x10000 + 0x4000,
    with the second chunk's address advanced by the first chunk's length."""
    res = _call_chunks(tmp_path, "0x20000d00", "0x14000")
    assert res.returncode == 0, res.stderr
    lines = res.stdout.strip().splitlines()
    assert lines == [
        "mem8 0x20000D00, 0x10000",
        "mem8 0x20010D00, 0x4000",
    ]


@_NEEDS_BASH
def test_chunk_count_matches_ceil_division(tmp_path: Path) -> None:
    """General ceil(size / 0x10000) count, not just the issue's one worked
    example -- three full chunks plus a partial fourth."""
    size = 0x10000 * 3 + 0x123
    res = _call_chunks(tmp_path, "0x0", hex(size))
    assert res.returncode == 0, res.stderr
    lines = res.stdout.strip().splitlines()
    assert len(lines) == 4
    assert lines[0] == "mem8 0x0, 0x10000"
    assert lines[1] == "mem8 0x10000, 0x10000"
    assert lines[2] == "mem8 0x20000, 0x10000"
    assert lines[3] == "mem8 0x30000, 0x123"


@_NEEDS_BASH
def test_chunk_bare_size_is_treated_as_hex(tmp_path: Path) -> None:
    """alp-sdk#2313 regression: JLinkExe's own `mem8` command -- and
    ram-run.sh's `[bufsize_hex]` usage -- always treated a bare (no `0x`)
    size as HEX. `$((size))` on a bare `1000` silently reparses it as
    DECIMAL instead; a bare size must still resolve to the hex value."""
    res = _call_chunks(tmp_path, "0x20000d00", "1000")
    assert res.returncode == 0, res.stderr
    assert res.stdout.strip() == "mem8 0x20000D00, 0x1000"


@_NEEDS_BASH
def test_chunk_bare_size_with_hex_only_digit_does_not_error(tmp_path: Path) -> None:
    """A bare size containing a hex-only digit (`1A00`) used to hit bash
    arithmetic's base-10 default and error outright ("value too great for
    base"); it must resolve to 0x1A00, not fail."""
    res = _call_chunks(tmp_path, "0x0", "1A00")
    assert res.returncode == 0, res.stderr
    assert res.stdout.strip() == "mem8 0x0, 0x1A00"


@_NEEDS_BASH
def test_chunk_zero_size_is_a_hard_error(tmp_path: Path) -> None:
    res = _call_chunks(tmp_path, "0x20000d00", "0")
    assert res.returncode == 1
    assert "size must be > 0" in res.stderr


@_NEEDS_BASH
def test_chunk_non_hex_size_gets_the_custom_error_not_a_bash_arithmetic_one(
    tmp_path: Path,
) -> None:
    """A size that is not even hex ('zz') must be caught by the explicit
    `^(0[xX])?[0-9A-Fa-f]+$` check before `$(( ))` ever sees it, and get
    THIS function's own named error, not bash arithmetic's generic one."""
    res = _call_chunks(tmp_path, "0x20000d00", "zz")
    assert res.returncode == 1
    assert "size 'zz' is not a valid hex value" in res.stderr


@_NEEDS_BASH
def test_chunk_bad_addr_is_a_hard_error(tmp_path: Path) -> None:
    """A genuinely malformed arithmetic expression (two tokens, no operator)
    must be refused, not silently coerced to 0. A bare non-numeric WORD is
    not a reliable negative here -- bash arithmetic treats an unset bare
    identifier as a variable reference and evaluates it to 0, not an error."""
    res = _call_chunks(tmp_path, "0x100 0x200", "0x100")
    assert res.returncode == 1


def test_ram_run_and_reread_route_their_mem8_through_the_chunker() -> None:
    """The two call sites named in the issue must build their CommandFile
    from bench_mem8_chunks(), not a hand-written 'mem8 $BUF, $SIZE' line --
    a raw line reintroduces the >0x10000 rejection silently."""
    for path in (RAM_RUN, REREAD):
        body = path.read_text(encoding="utf-8")
        assert "bench_mem8_chunks" in body, f"{path.name} does not call bench_mem8_chunks"
        assert not any(
            line.strip().startswith("mem8 $BUF") or line.strip().startswith("mem8 \"$BUF\"")
            for line in body.splitlines()
        ), f"{path.name} still has a raw, unchunked 'mem8 $BUF' line"


def test_ram_run_and_reread_both_call_the_shared_chunk_verify_guard() -> None:
    """ram-run.sh and reread.sh must both route their read-failure check
    through the ONE shared `bench_mem8_verify_chunks()` helper (bench-env.sh)
    rather than each carrying its own copy -- reread.sh had NO such check at
    all before this fix (alp-sdk#2313 item 3)."""
    for path in (RAM_RUN, REREAD):
        body = path.read_text(encoding="utf-8")
        assert "bench_mem8_verify_chunks" in body, (
            f"{path.name} does not call the shared bench_mem8_verify_chunks() guard")


def _call_verify_chunks(
    tmp_path: Path, label: str, transcript_body: str, mem8_lines: str,
) -> subprocess.CompletedProcess[str]:
    """Source bench-env.sh (no labgrid inputs, so it never reaches real
    infrastructure) and call bench_mem8_verify_chunks() directly against a
    hand-built transcript -- the same function ram-run.sh and reread.sh both
    call after their own JLinkExe session completes."""
    (tmp_path / "bench-env.sh").write_bytes(ENV.read_bytes())
    transcript = tmp_path / "transcript.out"
    transcript.write_text(transcript_body, encoding="utf-8")
    script = (
        "unset LG_PLACE LG_COORDINATOR LG_SWD_PATH ALP_JLINK_SEARCH_ROOT\n"
        "source ./bench-env.sh\n"
        f'bench_mem8_verify_chunks "{label}" "{transcript}" "{mem8_lines}"\n'
        "exit $?\n"
    )
    gate = tmp_path / "gate.sh"
    gate.write_text(script, encoding="utf-8")
    return subprocess.run(
        ["bash", "gate.sh"], cwd=tmp_path, capture_output=True,
        text=True, encoding="utf-8", errors="replace", timeout=60,
    )


@_NEEDS_BASH
def test_mem8_verify_chunks_passes_when_every_chunk_dumps(tmp_path: Path) -> None:
    mem8_lines = "mem8 0x1000, 0x10000\nmem8 0x11000, 0x4000"
    transcript = "1000 = 68 69 00 00\n11000 = 68 69 00 00\n"
    res = _call_verify_chunks(tmp_path, "reread", transcript, mem8_lines)
    assert res.returncode == 0, res.stderr


@_NEEDS_BASH
def test_mem8_verify_chunks_could_not_read_memory_is_a_hard_error(tmp_path: Path) -> None:
    mem8_lines = "mem8 0x1000, 0x10000"
    transcript = "1000 = 68 69 00 00\nCould not read memory.\n"
    res = _call_verify_chunks(tmp_path, "reread", transcript, mem8_lines)
    assert res.returncode == 9
    assert "!! reread: mem8 reported 'Could not read memory'" in res.stderr


@_NEEDS_BASH
def test_mem8_verify_chunks_no_dump_at_all_is_a_hard_error(tmp_path: Path) -> None:
    mem8_lines = "mem8 0x1000, 0x10000"
    res = _call_verify_chunks(tmp_path, "reread", "", mem8_lines)
    assert res.returncode == 9
    assert "!! reread: no memory dump line in the read session's transcript" in res.stderr


@_NEEDS_BASH
def test_mem8_verify_chunks_a_dropped_chunk_is_a_hard_error(tmp_path: Path) -> None:
    """reread.sh's guard, exercised the same way as ram-run.sh's own
    end-to-end dropped-chunk test: the SECOND of two chunks has no dump
    line while the first still comes back -- the whole-transcript
    'any dump line at all' check would not catch this on its own."""
    mem8_lines = "mem8 0x1000, 0x10000\nmem8 0x11000, 0x4000"
    transcript = "1000 = 68 69 00 00\n"   # chunk 2 (0x11000) silently missing
    res = _call_verify_chunks(tmp_path, "reread", transcript, mem8_lines)
    assert res.returncode == 9
    assert "!! reread: no dump line for chunk 'mem8 0x11000, 0x4000'" in res.stderr


# --- openocd-ram-run.sh: NVIC ICER/ICPR clear before resume (HE path) ------

def test_openocd_ram_run_clears_nvic_icer_and_icpr() -> None:
    """The 16 ICER (0xE000E180 + 4n) and 16 ICPR (0xE000E280 + 4n)
    addresses, n in 0..15 (IRQ 0-511, comfortably past the E8's highest IRQ,
    480), are built by a runtime loop (not baked in as 32 literals) -- assert
    the loop bounds and base addresses, then actually RUN the loop the way
    the script does and confirm each computed 'mww' write is embedded in the
    generated CMDS array, by sourcing the script's own computation."""
    body = OPENOCD_RAM_RUN.read_text(encoding="utf-8")

    assert "0xE000E180" in body, "missing the NVIC ICER base address"
    assert "0xE000E280" in body, "missing the NVIC ICPR base address"
    assert "seq 0 15" in body, "loop bound must cover n=0..15 (IRQ 0-511)"
    assert "0xFFFFFFFF" in body, "must write-1-to-clear (0xFFFFFFFF), not merely read"

    # The clear must be built strictly between 'halt' and 'resume' in the
    # CMDS construction -- built after the halt (so the core is stopped
    # while NVIC state is cleared) and strictly before 'resume' (an NVIC
    # clear issued after IRQs are already unmasked is too late).
    halt_idx = body.index('-c "halt"')
    icer_idx = body.index('"${NVIC_CLEAR_CMDS[@]}"')
    resume_idx = body.index('-c "resume"')
    assert halt_idx < icer_idx < resume_idx, (
        "NVIC ICER/ICPR clear must be embedded strictly between 'halt' "
        "and 'resume' in the CMDS construction")


@_NEEDS_BASH
def test_openocd_ram_run_nvic_loop_actually_generates_all_32_addresses(tmp_path: Path) -> None:
    """Behavioural companion to the static check above: extract and run the
    exact NVIC_CLEAR_CMDS-building loop from the real script (not a
    reimplementation) and confirm it produces the full ICER/ICPR range."""
    body = OPENOCD_RAM_RUN.read_text(encoding="utf-8")
    start = body.index("NVIC_CLEAR_CMDS=()")
    end = body.index("unset _n _icer_addr _icpr_addr") + len("unset _n _icer_addr _icpr_addr")
    loop_src = body[start:end]

    script = tmp_path / "gate.sh"
    script.write_text(
        loop_src + '\nprintf "%s\\n" "${NVIC_CLEAR_CMDS[@]}"\n',
        encoding="utf-8",
    )
    res = subprocess.run(
        ["bash", str(script)], capture_output=True, text=True,
        encoding="utf-8", errors="replace", timeout=30,
    )
    assert res.returncode == 0, res.stderr
    lines = res.stdout.splitlines()
    mww_lines = [ln for ln in lines if ln.startswith("mww ")]
    assert len(mww_lines) == 32, f"expected 32 'mww' entries (16 ICER + 16 ICPR), got {len(mww_lines)}"
    lines = mww_lines
    for n in range(16):
        icer_addr = f"0x{0xE000E180 + 4 * n:X}"
        icpr_addr = f"0x{0xE000E280 + 4 * n:X}"
        assert f"mww {icer_addr} 0xFFFFFFFF" in lines, f"missing ICER clear at {icer_addr}"
        assert f"mww {icpr_addr} 0xFFFFFFFF" in lines, f"missing ICPR clear at {icpr_addr}"


def test_openocd_ram_run_still_clears_msplim_and_psplim() -> None:
    """Regression guard: the NVIC fix must not have displaced the existing
    MSPLIM/PSPLIM clears (both secure and non-secure banks)."""
    body = OPENOCD_RAM_RUN.read_text(encoding="utf-8")
    for reg in ("msplim_s", "msplim_ns", "psplim_s", "psplim_ns"):
        assert f'reg {reg} 0x00000000' in body, f"missing '{reg}' clear"


def test_openocd_ram_run_nvic_clear_applies_to_both_core_paths() -> None:
    """CMDS (shared by CORE=hp and CORE=he) must embed the NVIC clear array
    -- the fix is not HE-only-conditional, since the shared CMDS list is
    used for both cores (see the script's own SELECT_CMDS split)."""
    body = OPENOCD_RAM_RUN.read_text(encoding="utf-8")
    # The NVIC_CLEAR_CMDS array must be both built AND embedded into CMDS.
    assert "NVIC_CLEAR_CMDS=()" in body
    assert '"${NVIC_CLEAR_CMDS[@]}"' in body
