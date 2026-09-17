#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Generate the 64-byte Secure Data Page mirror binary for the onsemi
N24S128's second device-select header (I2C address 0x58, first
pointer byte / selector 0x00 -- see
alp-sdk-internal/EEPROM-MANIFEST-SPEC.md's "Secure Data Page mirror
layout" section).

A SEPARATE script from program_eeprom.py, not a mode of it: the two
files transcribe unrelated, differently-sized field-width constants
(this file's SKU_LEN/HW_REV_LEN/SERIAL_LEN describe the 64-byte
mirror; program_eeprom.py's describe the 128-byte array manifest),
and scripts/check_board_id_doc_parity.py resolves program_eeprom.py's
five constant names by exact match -- adding a second set to that
file risks either colliding with that gate's parser or silently
being checked against the wrong struct's width.

Reads a project board.yaml the same way program_eeprom.py does (SKU,
family, hw_rev) -- imported from that module rather than
re-implemented -- and takes the factory serial + manufacturing date
as CLI args, exactly mirroring that script's interface so a
provisioning run can generate both blobs from the same inputs.

The byte layout matches `alp_secure_page_mirror_t` in
`include/alp/hw_info.h` and is enforced by a runtime check + that
header's `_Static_assert(sizeof(...) == 64, ...)`.

Usage:

    python3 scripts/program_eeprom_secure_page.py \\
        --board-yaml board.yaml \\
        --serial 2026W19-0001 \\
        --mfg-date 2026-05-11 \\
        --output build/secure-page.bin

The output is exactly 64 bytes.  This blob is NOT the array manifest
-- it goes to selector 0x00 of the SEPARATE 0x58 address space, and
only after the array manifest itself has already been written,
cold-cycled, and verified (docs/som-batch-provisioning-procedure.md
§7 steps 3-5 in alp-sdk-internal).  Never write it to offset 0 of the
0x50 array.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

# Reuse the board.yaml / preset resolution logic verbatim rather than
# re-implementing it -- see this file's own docstring for why the
# 64-byte struct's field-width constants themselves are NOT imported
# (check_board_id_doc_parity.py's parser only knows program_eeprom.py's
# five names).
sys.path.insert(0, str(Path(__file__).resolve().parent))
import program_eeprom as _manifest  # noqa: E402  (path insert must run first)


MAGIC = 0x414C5350          # 'ALSP' -- deliberately different from the
                             # array manifest's 'ALPH' (0x414C5048), so the
                             # two 24C128 address spaces can never be
                             # mistaken for each other by a magic check.
SCHEMA_VERSION = 1

# Field length budgets -- must match ALP_SECURE_PAGE_*_LEN in
# include/alp/hw_info.h.  Deliberately NOT the same widths as
# program_eeprom.py's FAMILY_LEN/SKU_LEN/HW_REV_LEN/SERIAL_LEN --
# see EEPROM-MANIFEST-SPEC.md's Secure Data Page section for why.
# sku gets the most headroom of the three (16 against a 10-char pattern,
# matching the array manifest's own 24-byte precedent for the SAME field)
# because it is what this mirror exists to recover; hw_rev gets the least
# (12 against 7 chars) because it tracks a physical Altium board revision,
# the field least likely to ever need more room.
SKU_LEN = 16
HW_REV_LEN = 12
SERIAL_LEN = 23

MIRROR_SIZE = 64

# Struct format -- little-endian, exact-width, packed (no padding).
#
#   uint32_t magic                4
#   uint8_t  schema_version       1
#   char[16] sku                 16
#   char[12] hw_rev              12
#   char[23] serial              23
#   uint16_t mfg_year             2
#   uint8_t  mfg_month            1
#   uint8_t  mfg_day              1
#   uint32_t crc32                4
#                                ----
#                                 64
_HEADER_FMT = f"<I B {SKU_LEN}s {HW_REV_LEN}s {SERIAL_LEN}s H B B"
_HEADER_SIZE = struct.calcsize(_HEADER_FMT)
_CRC_FMT = "<I"

assert _HEADER_SIZE + struct.calcsize(_CRC_FMT) == MIRROR_SIZE, (
    f"secure-page mirror layout drift: {_HEADER_SIZE} + 4 != {MIRROR_SIZE}"
)


def _build_mirror(sku: str, hw_rev: str, serial: str, mfg) -> bytes:
    header = struct.pack(
        _HEADER_FMT,
        MAGIC,
        SCHEMA_VERSION,
        _manifest._check_string("sku", sku, SKU_LEN),
        _manifest._check_string("hw_rev", hw_rev, HW_REV_LEN),
        _manifest._check_string("serial", serial, SERIAL_LEN),
        mfg.year,
        mfg.month,
        mfg.day,
    )
    crc = zlib.crc32(header) & 0xFFFFFFFF
    return header + struct.pack(_CRC_FMT, crc)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Pack a board.yaml + serial + mfg date into the "
                    "64-byte Secure Data Page mirror binary.")
    parser.add_argument("--board-yaml", type=Path, default=Path("board.yaml"),
                        help="Path to the project's board.yaml (default: ./board.yaml).")
    parser.add_argument("--serial", required=True,
                        help="Factory-assigned serial number (max 22 ASCII chars).")
    parser.add_argument("--mfg-date", required=True,
                        help="Manufacturing date in ISO format (YYYY-MM-DD).")
    parser.add_argument("--output", type=Path, default=Path("secure-page.bin"),
                        help="Output path for the 64-byte binary (default: ./secure-page.bin).")
    parser.add_argument("--metadata-root", type=Path,
                        default=Path(__file__).resolve().parent.parent / "metadata",
                        help="Override the metadata search root.")
    args = parser.parse_args()

    project = _manifest._load_board_yaml(args.board_yaml)
    sku = (project.get("som") or {}).get("sku")
    if not sku:
        sys.exit("program_eeprom_secure_page: board.yaml has no som.sku")
    # family is only needed transiently, to resolve the board datecode that
    # gets composed into hw_rev below -- it is NOT stored in the mirror
    # (see include/alp/hw_info.h's alp_secure_page_mirror_t doc comment for
    # why: derivable from the SKU, and there is no spare room).
    family = _manifest._resolve_family(sku, args.metadata_root)
    hw_rev = _manifest._resolve_hw_rev(project, args.metadata_root)
    mfg = _manifest._parse_date(args.mfg_date)

    datecode = _manifest._resolve_board_datecode(family, args.metadata_root)
    hw_rev_field = f"{datecode}-{hw_rev}" if datecode else hw_rev

    blob = _build_mirror(sku, hw_rev_field, args.serial, mfg)
    if len(blob) != MIRROR_SIZE:
        sys.exit(f"program_eeprom_secure_page: mirror is {len(blob)} bytes, "
                 f"expected {MIRROR_SIZE} -- header layout bug")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    print(f"program_eeprom_secure_page: wrote {len(blob)} bytes to {args.output}")
    print(f"  sku      {sku}")
    print(f"  hw_rev   {hw_rev_field}")
    print(f"  serial   {args.serial}")
    print(f"  mfg_date {mfg.isoformat()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
