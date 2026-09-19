#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Parse the AEN E8 SETOOLS ATOC trailer (alp-sdk#2233 review blocker 2).

WHY THIS EXISTS
----------------
`erase-storage.sh` (#1430) erases the customer storage window immediately
BELOW the SE-owned `atoc` region using the metadata preset's window
boundaries alone. Those boundaries describe the ALLOCATED `atoc` region, not
where the signed ATOC package SETOOLS actually wrote ends up -- and on a real
E1M-AEN803 bench board, serial 2026W36-0001 (2026-09-19), the resident ATOC
package was measured to start well INSIDE that allocated region, at
`0x8056A3C0`, not at its base (`0x80578000`). A metadata-window-only erase on
that board would zero `0x8056A3C0`-`0x80577FFF` of a live, no-SE-UART-recovery
boot table.

WHAT WAS MEASURED (BENCH-MEASURED, ONE BOARD, 2026-09-19 -- NOT a documented
SETOOLS/Alif spec; treat every field below as a HINT this module validates,
never as ground truth taken on faith)
-----------------------------------------------------------------------
SETOOLS top-anchors the signed ATOC at the top of its allocated window and
grows it downward. The last 16 bytes of that window held three little-endian
32-bit words -- this module calls that 16-byte tail the "trailer":

    +0x0  header_address   -- where the `OEMTOC01` signature itself lives
                              (measured: 0x8057FF90, i.e. window_end - 0x70)
    +0x4  package_start    -- the LOWEST address SETOOLS actually wrote
                              (measured: 0x8056A3C0)
    +0x8  package_size     -- package_start + package_size == window_end
                              (measured: 0x00015C40; 0x8056A3C0 + 0x15C40
                              == 0x80580000, the window's own end)

A trailer that is all-`0x00` or all-`0xFF`, with no `OEMTOC01` signature
found, means no resident ATOC -- the window is genuinely blank. Anything
else that fails the consistency checks below is refused outright: this
module never guesses a package location it cannot verify.
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass

TRAILER_SIZE = 16
HEADER_SIGNATURE = b"OEMTOC01"
HEADER_SIGNATURE_SIZE = len(HEADER_SIGNATURE)


class AtocTrailerError(Exception):
    """Refusal: the trailer is missing, unreadable, or internally inconsistent."""


@dataclass
class AtocTrailer:
    header_address: int
    package_start: int
    package_size: int

    @property
    def package_end(self) -> int:  # exclusive
        return self.package_start + self.package_size


def parse_trailer_bytes(data: bytes) -> AtocTrailer | None:
    """The 16-byte trailer -> AtocTrailer, or None if it reads as "no
    resident ATOC" (all-0x00 or all-0xFF). Raises on a malformed length --
    the CALLER is responsible for slicing exactly TRAILER_SIZE bytes from
    the right offset; this only ever sees what it's handed."""
    if len(data) != TRAILER_SIZE:
        raise AtocTrailerError(f"trailer must be exactly {TRAILER_SIZE} bytes, got {len(data)}")
    if data == b"\x00" * TRAILER_SIZE or data == b"\xff" * TRAILER_SIZE:
        return None
    header_addr, pkg_start, pkg_size = struct.unpack("<III", data[:12])
    return AtocTrailer(header_address=header_addr, package_start=pkg_start, package_size=pkg_size)


def validate_trailer(trailer: AtocTrailer, header_bytes: bytes, window_lo: int, window_end: int) -> None:
    """Refuses (AtocTrailerError) unless EVERY field checks out -- an
    inconsistent trailer means the package location cannot be safely
    determined, which is refused, not guessed at (alp-sdk#2233 review
    blocker 2: "REFUSE if the trailer is present but inconsistent")."""
    if not header_bytes.startswith(HEADER_SIGNATURE):
        raise AtocTrailerError(
            f"trailer names a header at 0x{trailer.header_address:08X} but the bytes there do not "
            f"start with {HEADER_SIGNATURE!r} (got {header_bytes[:HEADER_SIGNATURE_SIZE]!r}) -- "
            "cannot trust this trailer"
        )
    if trailer.package_end != window_end:
        raise AtocTrailerError(
            f"trailer's package_start (0x{trailer.package_start:08X}) + package_size "
            f"(0x{trailer.package_size:X}) = 0x{trailer.package_end:08X}, expected exactly "
            f"0x{window_end:08X} (the atoc region's own end) -- inconsistent trailer, cannot trust it"
        )
    if trailer.package_start < window_lo:
        raise AtocTrailerError(
            f"trailer's package_start 0x{trailer.package_start:08X} is below the MRAM window "
            f"0x{window_lo:08X} -- inconsistent trailer, cannot trust it"
        )


def read_bytes_from_sector(sector_path: str, sector_base: int, addr: int, size: int) -> bytes:
    """<size> bytes at <addr>, sliced out of one pre-read sector image
    (<sector_path>, covering [sector_base, sector_base + len)). Refuses if
    [addr, addr+size) is not entirely inside that one sector -- this module
    never stitches multiple sector files together; the caller must pre-read
    whichever sector(s) it needs and call this once per sector."""
    with open(sector_path, "rb") as f:
        sector_bytes = f.read()
    off = addr - sector_base
    if off < 0 or off + size > len(sector_bytes):
        raise AtocTrailerError(
            f"0x{addr:08X}+{size} is not inside the pre-read sector {sector_path} "
            f"(0x{sector_base:08X}, {len(sector_bytes)} B)"
        )
    return sector_bytes[off:off + size]


def cmd_resolve(args: argparse.Namespace) -> int:
    """Print `NO_ATOC` (blank window) or `PACKAGE_START=0x...` on success;
    REFUSE: <reason> on stderr and exit 1 on any inconsistency."""
    trailer_addr = args.window_end - TRAILER_SIZE
    trailer_bytes = read_bytes_from_sector(args.sector_file, args.sector_base, trailer_addr, TRAILER_SIZE)
    trailer = parse_trailer_bytes(trailer_bytes)
    if trailer is None:
        print("NO_ATOC")
        return 0

    header_bytes = read_bytes_from_sector(
        args.sector_file, args.sector_base, trailer.header_address, HEADER_SIGNATURE_SIZE
    )
    validate_trailer(trailer, header_bytes, args.window_lo, args.window_end)
    print(f"PACKAGE_START=0x{trailer.package_start:08X}")
    print(f"PACKAGE_SIZE=0x{trailer.package_size:X}")
    print(f"HEADER_ADDRESS=0x{trailer.header_address:08X}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_resolve = sub.add_parser(
        "resolve",
        help="resolve the resident ATOC package's start address from a pre-read sector image",
    )
    p_resolve.add_argument("--sector-file", required=True, help="pre-read image of the sector holding the trailer")
    p_resolve.add_argument("--sector-base", type=lambda s: int(s, 0), required=True)
    p_resolve.add_argument("--window-lo", type=lambda s: int(s, 0), required=True, help="MRAM window lo bound")
    p_resolve.add_argument(
        "--window-end", type=lambda s: int(s, 0), required=True,
        help="the atoc region's own end (exclusive) -- package_start + package_size must equal this",
    )
    p_resolve.set_defaults(func=cmd_resolve)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except AtocTrailerError as exc:
        print(f"REFUSE: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
