# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/bench/aen/atoc_trailer.py (alp-sdk#2233 review blocker 2).

Covers the trailer-parse cases the review named explicitly: a valid
bench-measured-shape trailer, a blank (all-0x00 / all-0xFF) "no resident
ATOC" trailer, and every inconsistency this module must REFUSE rather than
guess past (bad header signature, start+size != window_end, start below the
MRAM window).
"""
from __future__ import annotations

import importlib.util
import struct
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "bench" / "aen" / "atoc_trailer.py"

SECTOR = 0x4000
# The bench-measured layout (E1M-AEN803, serial 2026W36-0001, 2026-09-19):
# atoc region 0x80578000-0x8057FFFF (32 KiB), package starting well inside it.
# alp-sdk#2233 review round 5: re-measured with `mem32` twice plus a `savebin`
# tail agreeing on the same 16 bytes at 0x8057FFF0 --
#   0x4966A80E 8057FF90 8056A3C0 00015C40
# at trailer offsets +0x0/+0x4/+0x8/+0xC respectively. An earlier round had
# these offsets wrong by one word (three words starting at +0x0), which
# misparsed WORD0 as header_address and refused every real trailer.
SECTOR_BASE = 0x8057C000
WINDOW_END = 0x80580000
WINDOW_LO = 0x80000000
WORD0 = 0x4966A80E  # opaque, TBD -- possibly a checksum, never validated
HEADER_ADDR = 0x8057FF90
PKG_START = 0x8056A3C0
PKG_SIZE = 0x15C40
assert PKG_START + PKG_SIZE == WINDOW_END
# The EXACT measured 16 bytes at 0x8057FFF0, byte for byte -- used by
# test_cli_resolve_exact_measured_bytes below, independent of the
# struct.pack repacking every other fixture in this file uses.
MEASURED_TRAILER_BYTES = bytes.fromhex("0E A8 66 49 90 FF 57 80 C0 A3 56 80 40 5C 01 00".replace(" ", ""))
assert len(MEASURED_TRAILER_BYTES) == 16
assert struct.unpack("<IIII", MEASURED_TRAILER_BYTES) == (WORD0, HEADER_ADDR, PKG_START, PKG_SIZE)


@pytest.fixture(scope="module")
def mod():
    spec = importlib.util.spec_from_file_location("atoc_trailer", SCRIPT)
    assert spec is not None and spec.loader is not None
    m = importlib.util.module_from_spec(spec)
    sys.modules["atoc_trailer"] = m
    spec.loader.exec_module(m)
    return m


def _measured_sector(mutate=None) -> bytes:
    """One full sector carrying the bench-measured trailer + header,
    optionally mutated by `mutate(bytearray)` before returning."""
    buf = bytearray(SECTOR)
    buf[HEADER_ADDR - SECTOR_BASE: HEADER_ADDR - SECTOR_BASE + 8] = b"OEMTOC01"
    trailer_off = (WINDOW_END - 16) - SECTOR_BASE
    buf[trailer_off:trailer_off + 16] = struct.pack("<IIII", WORD0, HEADER_ADDR, PKG_START, PKG_SIZE)
    if mutate:
        mutate(buf)
    return bytes(buf)


def _run_cli(args: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], cwd=cwd,
        capture_output=True, text=True, encoding="utf-8", timeout=30,
    )


# --------------------------------------------------------------------
# Library-level (pure) tests
# --------------------------------------------------------------------

def test_parse_valid_measured_trailer(mod) -> None:
    data = _measured_sector()
    trailer_off = (WINDOW_END - 16) - SECTOR_BASE
    trailer = mod.parse_trailer_bytes(data[trailer_off:trailer_off + 16])
    assert trailer is not None
    assert trailer.word0 == WORD0
    assert trailer.header_address == HEADER_ADDR
    assert trailer.package_start == PKG_START
    assert trailer.package_size == PKG_SIZE
    assert trailer.package_end == WINDOW_END
    header = data[HEADER_ADDR - SECTOR_BASE: HEADER_ADDR - SECTOR_BASE + 8]
    mod.validate_trailer(trailer, header, WINDOW_LO, WINDOW_END)  # must not raise


def test_parse_blank_zero_trailer_is_no_atoc(mod) -> None:
    assert mod.parse_trailer_bytes(b"\x00" * 16) is None


def test_parse_blank_erased_trailer_is_no_atoc(mod) -> None:
    assert mod.parse_trailer_bytes(b"\xff" * 16) is None


def test_parse_wrong_length_refuses(mod) -> None:
    with pytest.raises(mod.AtocTrailerError, match="exactly 16 bytes"):
        mod.parse_trailer_bytes(b"\x00" * 15)


def test_validate_refuses_bad_header_signature(mod) -> None:
    trailer = mod.AtocTrailer(word0=WORD0, header_address=HEADER_ADDR, package_start=PKG_START, package_size=PKG_SIZE)
    with pytest.raises(mod.AtocTrailerError, match="do not start with"):
        mod.validate_trailer(trailer, b"GARBAGE!", WINDOW_LO, WINDOW_END)


def test_validate_refuses_start_plus_size_mismatch(mod) -> None:
    trailer = mod.AtocTrailer(word0=WORD0, header_address=HEADER_ADDR, package_start=PKG_START, package_size=PKG_SIZE - 0x100)
    with pytest.raises(mod.AtocTrailerError, match="inconsistent trailer"):
        mod.validate_trailer(trailer, b"OEMTOC01", WINDOW_LO, WINDOW_END)


def test_validate_refuses_start_below_window_lo(mod) -> None:
    trailer = mod.AtocTrailer(word0=WORD0, header_address=HEADER_ADDR, package_start=0x70000000, package_size=WINDOW_END - 0x70000000)
    with pytest.raises(mod.AtocTrailerError, match="below the MRAM window"):
        mod.validate_trailer(trailer, b"OEMTOC01", WINDOW_LO, WINDOW_END)


def test_validate_never_checks_word0(mod) -> None:
    """alp-sdk#2233 review round 5: word0 is opaque (possibly a checksum,
    UNKNOWN meaning) and must never be validated -- an otherwise-valid
    trailer with a garbage word0 must still pass."""
    trailer = mod.AtocTrailer(word0=0xDEADBEEF, header_address=HEADER_ADDR, package_start=PKG_START, package_size=PKG_SIZE)
    mod.validate_trailer(trailer, b"OEMTOC01", WINDOW_LO, WINDOW_END)  # must not raise


# alp-sdk#2233 review round 3, finding 6: a zero-size package, or a header
# address outside [package_start, package_end), is internally inconsistent
# the same way a bad checksum is -- refuse rather than trust it.

def test_validate_refuses_zero_size_package(mod) -> None:
    trailer = mod.AtocTrailer(word0=WORD0, header_address=WINDOW_END, package_start=WINDOW_END, package_size=0)
    with pytest.raises(mod.AtocTrailerError, match="package_size"):
        mod.validate_trailer(trailer, b"OEMTOC01", WINDOW_LO, WINDOW_END)


def test_validate_refuses_header_below_package_start(mod) -> None:
    # header_address sits BELOW package_start -- outside the package, even
    # though package_end still checks out.
    trailer = mod.AtocTrailer(word0=WORD0, header_address=PKG_START - 0x10, package_start=PKG_START, package_size=PKG_SIZE)
    with pytest.raises(mod.AtocTrailerError, match="BELOW package_start"):
        mod.validate_trailer(trailer, b"OEMTOC01", WINDOW_LO, WINDOW_END)


def test_validate_refuses_header_that_does_not_fit_before_package_end(mod) -> None:
    # header_address + 8 (the signature length) overruns package_end by one
    # byte -- the header does not fully fit inside the package it names.
    # package_end must still equal window_end (PKG_START + package_size) so
    # this exercises ONLY the new header-fit check, not the pre-existing
    # package_end != window_end one.
    trailer = mod.AtocTrailer(word0=WORD0, header_address=WINDOW_END - 7, package_start=PKG_START,
                               package_size=WINDOW_END - PKG_START)
    with pytest.raises(mod.AtocTrailerError, match="does not fit inside the package"):
        mod.validate_trailer(trailer, b"OEMTOC01", WINDOW_LO, WINDOW_END)


def test_validate_accepts_header_exactly_at_package_end_minus_signature(mod) -> None:
    # The boundary case: header_address + 8 == package_end exactly must be
    # ACCEPTED (the header's last byte is the package's last byte) -- only
    # spilling past it is refused.
    header_addr = WINDOW_END - 8
    trailer = mod.AtocTrailer(word0=WORD0, header_address=header_addr, package_start=PKG_START,
                               package_size=WINDOW_END - PKG_START)
    mod.validate_trailer(trailer, b"OEMTOC01", WINDOW_LO, WINDOW_END)  # must not raise


# --------------------------------------------------------------------
# CLI (subprocess) tests -- what erase-storage.sh actually invokes
# --------------------------------------------------------------------

def test_cli_resolve_valid_measured_layout(tmp_path: Path) -> None:
    p = tmp_path / "sector.bin"
    p.write_bytes(_measured_sector())
    res = _run_cli(
        ["resolve", "--sector-file", str(p), "--sector-base", hex(SECTOR_BASE),
         "--window-lo", hex(WINDOW_LO), "--window-end", hex(WINDOW_END)],
        cwd=tmp_path,
    )
    assert res.returncode == 0, res.stderr
    assert f"PACKAGE_START=0x{PKG_START:08X}" in res.stdout


def test_cli_resolve_blank_window_is_no_atoc(tmp_path: Path) -> None:
    p = tmp_path / "sector.bin"
    p.write_bytes(b"\x00" * SECTOR)
    res = _run_cli(
        ["resolve", "--sector-file", str(p), "--sector-base", hex(SECTOR_BASE),
         "--window-lo", hex(WINDOW_LO), "--window-end", hex(WINDOW_END)],
        cwd=tmp_path,
    )
    assert res.returncode == 0, res.stderr
    assert res.stdout.strip() == "NO_ATOC"


def test_cli_resolve_erased_0xff_window_is_no_atoc(tmp_path: Path) -> None:
    p = tmp_path / "sector.bin"
    p.write_bytes(b"\xff" * SECTOR)
    res = _run_cli(
        ["resolve", "--sector-file", str(p), "--sector-base", hex(SECTOR_BASE),
         "--window-lo", hex(WINDOW_LO), "--window-end", hex(WINDOW_END)],
        cwd=tmp_path,
    )
    assert res.returncode == 0, res.stderr
    assert res.stdout.strip() == "NO_ATOC"


def test_cli_resolve_refuses_bad_signature(tmp_path: Path) -> None:
    def mutate(buf: bytearray) -> None:
        buf[HEADER_ADDR - SECTOR_BASE: HEADER_ADDR - SECTOR_BASE + 8] = b"NOTATOC!"
    p = tmp_path / "sector.bin"
    p.write_bytes(_measured_sector(mutate))
    res = _run_cli(
        ["resolve", "--sector-file", str(p), "--sector-base", hex(SECTOR_BASE),
         "--window-lo", hex(WINDOW_LO), "--window-end", hex(WINDOW_END)],
        cwd=tmp_path,
    )
    assert res.returncode == 1
    assert "REFUSE" in res.stderr


# alp-sdk#2233 review round 3, finding 5: a blank TRAILER (all-0x00) does
# not by itself prove there is no resident ATOC if the OEMTOC01 signature is
# still findable elsewhere in the pre-read region -- an earlier version
# returned NO_ATOC without ever searching, contradicting its own docstring.

def test_cli_resolve_blank_trailer_refuses_when_signature_present_elsewhere(tmp_path: Path) -> None:
    buf = bytearray(SECTOR)  # trailer stays all-0x00 -> parses as blank
    # A live OEMTOC01 signature sitting at the SAME header offset a real
    # resident package would use, but with a blank trailer -- exactly the
    # adversarial case alp-sdk#2233 review round 3's harness constructs.
    buf[HEADER_ADDR - SECTOR_BASE: HEADER_ADDR - SECTOR_BASE + 8] = b"OEMTOC01"
    p = tmp_path / "sector.bin"
    p.write_bytes(bytes(buf))
    res = _run_cli(
        ["resolve", "--sector-file", str(p), "--sector-base", hex(SECTOR_BASE),
         "--window-lo", hex(WINDOW_LO), "--window-end", hex(WINDOW_END)],
        cwd=tmp_path,
    )
    assert res.returncode == 1, res.stdout + res.stderr
    assert "REFUSE" in res.stderr
    assert "NO_ATOC" not in res.stdout


def test_cli_resolve_blank_trailer_with_no_signature_anywhere_is_still_no_atoc(tmp_path: Path) -> None:
    # Control for the test above: a GENUINELY blank region (no signature
    # anywhere) must still proceed as NO_ATOC -- the fix must not turn every
    # blank window into a refusal.
    p = tmp_path / "sector.bin"
    p.write_bytes(b"\x00" * SECTOR)
    res = _run_cli(
        ["resolve", "--sector-file", str(p), "--sector-base", hex(SECTOR_BASE),
         "--window-lo", hex(WINDOW_LO), "--window-end", hex(WINDOW_END)],
        cwd=tmp_path,
    )
    assert res.returncode == 0, res.stdout + res.stderr
    assert res.stdout.strip() == "NO_ATOC"


def test_cli_resolve_refuses_inconsistent_size(tmp_path: Path) -> None:
    def mutate(buf: bytearray) -> None:
        trailer_off = (WINDOW_END - 16) - SECTOR_BASE
        buf[trailer_off:trailer_off + 16] = struct.pack("<IIII", WORD0, HEADER_ADDR, PKG_START, PKG_SIZE - 4)
    p = tmp_path / "sector.bin"
    p.write_bytes(_measured_sector(mutate))
    res = _run_cli(
        ["resolve", "--sector-file", str(p), "--sector-base", hex(SECTOR_BASE),
         "--window-lo", hex(WINDOW_LO), "--window-end", hex(WINDOW_END)],
        cwd=tmp_path,
    )
    assert res.returncode == 1
    assert "REFUSE" in res.stderr
    assert "inconsistent trailer" in res.stderr


# --------------------------------------------------------------------
# alp-sdk#2233 review round 5 -- the trailer-offset bug found on real
# silicon (E1M-AEN803, serial 2026W36-0001, 2026-09-19): an earlier version
# read header_address/package_start/package_size starting at trailer +0x0
# (three words), which on the ACTUAL layout (word0 at +0x0, then the three
# real fields at +0x4/+0x8/+0xC) misparses word0 as header_address and
# refuses every real trailer -- `erase-storage.sh --check-only` exited 6
# ("undeterminable") instead of the expected 7 (overlap) on real hardware.
# --------------------------------------------------------------------

def test_cli_resolve_exact_measured_bytes(tmp_path: Path) -> None:
    """Built from the EXACT measured 16 bytes (not repacked via struct),
    byte for byte, so a future offset/endianness slip in the fixture itself
    can never mask a real regression the way a repacked fixture could."""
    buf = bytearray(SECTOR)
    buf[HEADER_ADDR - SECTOR_BASE: HEADER_ADDR - SECTOR_BASE + 8] = b"OEMTOC01"
    trailer_off = (WINDOW_END - 16) - SECTOR_BASE
    buf[trailer_off:trailer_off + 16] = MEASURED_TRAILER_BYTES
    p = tmp_path / "sector.bin"
    p.write_bytes(bytes(buf))
    res = _run_cli(
        ["resolve", "--sector-file", str(p), "--sector-base", hex(SECTOR_BASE),
         "--window-lo", hex(WINDOW_LO), "--window-end", hex(WINDOW_END)],
        cwd=tmp_path,
    )
    assert res.returncode == 0, res.stdout + res.stderr
    assert f"PACKAGE_START=0x{PKG_START:08X}" in res.stdout
    assert f"PACKAGE_SIZE=0x{PKG_SIZE:X}" in res.stdout


def test_cli_resolve_refuses_the_old_wrong_three_word_layout(tmp_path: Path) -> None:
    """The OLD (wrong-by-one-word) layout: header_address/package_start/
    package_size packed starting at +0x0, with nothing meaningful in the
    trailing 4 bytes -- the shape an earlier version of this module
    expected. Reading it under the CORRECT (current) offsets must NEVER be
    misparsed into a false "proceed": it must refuse."""
    buf = bytearray(SECTOR)
    buf[HEADER_ADDR - SECTOR_BASE: HEADER_ADDR - SECTOR_BASE + 8] = b"OEMTOC01"
    trailer_off = (WINDOW_END - 16) - SECTOR_BASE
    # OLD layout: no word0 slot at all -- three words start at +0x0, leaving
    # the real trailer's own +0xC (package_size) slot as zero padding.
    buf[trailer_off:trailer_off + 12] = struct.pack("<III", HEADER_ADDR, PKG_START, PKG_SIZE)
    p = tmp_path / "sector.bin"
    p.write_bytes(bytes(buf))
    res = _run_cli(
        ["resolve", "--sector-file", str(p), "--sector-base", hex(SECTOR_BASE),
         "--window-lo", hex(WINDOW_LO), "--window-end", hex(WINDOW_END)],
        cwd=tmp_path,
    )
    assert res.returncode == 1, res.stdout + res.stderr
    assert "REFUSE" in res.stderr
    assert "PACKAGE_START" not in res.stdout, "must never resolve a false package start off the wrong offsets"
