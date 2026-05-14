#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Generate the 128-byte EEPROM manifest binary for the ALP SDK's
on-module 24C128 EEPROM (AEN family, sits at I2C address 0x50 per
the on_module section of metadata/e1m_modules/<MPN>.yaml).

Reads a project board.yaml for the SoM SKU + hw_rev + family, takes
the factory serial number + manufacturing date as CLI args, and
emits the binary the production-test fixture writes at offset
0x0000 of the EEPROM.

The byte layout matches `alp_hw_info_eeprom_t` in
`include/alp/hw_info.h` and is enforced by a runtime check + the
header's `_Static_assert(sizeof(...) == 128, ...)`.

Usage:

    python3 scripts/program_eeprom.py \\
        --board-yaml board.yaml \\
        --serial 2026W19-0001 \\
        --mfg-date 2026-05-11 \\
        --output build/eeprom-manifest.bin

The output is exactly 128 bytes.  Pipe it to the production-test
fixture's I2C writer; the SDK reads it back at boot via
`alp_hw_info_read()`.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from datetime import date
from pathlib import Path
from typing import Any

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("program_eeprom: PyYAML is required.  Install via `pip install pyyaml`.")


MAGIC = 0x414C5048   # 'ALPH'
SCHEMA_VERSION = 1

# Field length budgets -- must match the constants in
# include/alp/hw_info.h.  A drift between this file and the header
# is caught by the manifest size assertion below + by the C
# _Static_assert at compile time.
FAMILY_LEN = 16
SKU_LEN = 24
HW_REV_LEN = 8
SERIAL_LEN = 24
RESERVED_LEN = 40

MANIFEST_SIZE = 128

# Struct format -- little-endian, exact-width.  Mirror of
# alp_hw_info_eeprom_t.  Note: structs are packed (no padding),
# strings include their null terminator.
#
#   uint32_t magic               4
#   uint32_t schema_version      4
#   char[16] family             16
#   char[24] sku                24
#   char[8]  hw_rev              8
#   char[24] serial             24
#   uint16_t mfg_year            2
#   uint8_t  mfg_month           1
#   uint8_t  mfg_day             1
#   uint8_t  reserved[40]       40
#   uint32_t crc32               4
#                              ----
#                              128
_HEADER_FMT = (
    f"<I I {FAMILY_LEN}s {SKU_LEN}s {HW_REV_LEN}s {SERIAL_LEN}s "
    f"H B B {RESERVED_LEN}s"
)
_HEADER_SIZE = struct.calcsize(_HEADER_FMT)
_CRC_FMT = "<I"

assert _HEADER_SIZE + struct.calcsize(_CRC_FMT) == MANIFEST_SIZE, (
    f"manifest layout drift: {_HEADER_SIZE} + 4 != {MANIFEST_SIZE}"
)


def _check_string(name: str, value: str, max_len_incl_null: int) -> bytes:
    """Encode a string as fixed-length null-terminated bytes, or fail
    with a clear message if it doesn't fit."""
    encoded = value.encode("ascii")
    # The field is null-terminated, so we lose 1 byte to '\0'.
    if len(encoded) > max_len_incl_null - 1:
        sys.exit(f"program_eeprom: {name} '{value}' exceeds "
                 f"{max_len_incl_null - 1}-byte budget (got {len(encoded)})")
    return encoded + b"\0" * (max_len_incl_null - len(encoded))


def _load_board_yaml(path: Path) -> dict[str, Any]:
    if not path.is_file():
        sys.exit(f"program_eeprom: board.yaml not found at {path}")
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(doc, dict):
        sys.exit(f"program_eeprom: {path} did not parse to a mapping")
    return doc


def _resolve_family(sku: str, metadata_root: Path) -> str:
    """Find the family slug for an MPN by reading the SKU preset."""
    preset = metadata_root / "e1m_modules" / f"{sku}.yaml"
    if not preset.is_file():
        sys.exit(f"program_eeprom: no preset for {sku} at "
                 f"{preset} -- the production-test flow needs a "
                 f"shipped MPN preset to derive the family slug")
    doc = yaml.safe_load(preset.read_text(encoding="utf-8")) or {}
    family = doc.get("family", "")
    # Map the preset's vendor-style family name to the slug used
    # in metadata/e1m_modules/<family>/.  Mirror of the table in
    # scripts/validate_board_yaml.py.
    family_dir = {
        "alif-ensemble": "aen",
        "renesas-rzv2n": "v2n",
        "renesas-rzv2n-deepx": "v2n-m1",
        "nxp-imx9":  "imx93",
    }.get(family, family)
    if not family_dir:
        sys.exit(f"program_eeprom: {sku}'s preset has no `family:` field")
    return family_dir


def _resolve_hw_rev(project: dict[str, Any], metadata_root: Path) -> str:
    """Pick the explicit hw_rev or fall back to the SKU preset's
    default_hw_rev.  Same logic the loader applies."""
    declared = (project.get("som") or {}).get("hw_rev")
    if declared:
        return str(declared)
    sku = project["som"]["sku"]
    preset = metadata_root / "e1m_modules" / f"{sku}.yaml"
    doc = yaml.safe_load(preset.read_text(encoding="utf-8")) or {}
    default = doc.get("default_hw_rev")
    if not default:
        sys.exit(f"program_eeprom: board.yaml omits som.hw_rev "
                 f"and the {sku} preset has no default_hw_rev")
    return str(default)


def _parse_date(s: str) -> date:
    try:
        return date.fromisoformat(s)
    except ValueError as e:
        sys.exit(f"program_eeprom: --mfg-date '{s}' is not "
                 f"YYYY-MM-DD ({e})")


def _build_manifest(family: str, sku: str, hw_rev: str,
                    serial: str, mfg: date) -> bytes:
    header = struct.pack(
        _HEADER_FMT,
        MAGIC,
        SCHEMA_VERSION,
        _check_string("family", family, FAMILY_LEN),
        _check_string("sku", sku, SKU_LEN),
        _check_string("hw_rev", hw_rev, HW_REV_LEN),
        _check_string("serial", serial, SERIAL_LEN),
        mfg.year,
        mfg.month,
        mfg.day,
        b"\0" * RESERVED_LEN,
    )
    crc = zlib.crc32(header) & 0xFFFFFFFF
    return header + struct.pack(_CRC_FMT, crc)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Pack a board.yaml + serial + mfg date into the "
                    "128-byte ALP SDK EEPROM manifest binary.")
    parser.add_argument("--board-yaml", type=Path, default=Path("board.yaml"),
                        help="Path to the project's board.yaml (default: ./board.yaml).")
    parser.add_argument("--serial", required=True,
                        help="Factory-assigned serial number (max 23 ASCII chars).")
    parser.add_argument("--mfg-date", required=True,
                        help="Manufacturing date in ISO format (YYYY-MM-DD).")
    parser.add_argument("--output", type=Path, default=Path("eeprom-manifest.bin"),
                        help="Output path for the 128-byte binary (default: ./eeprom-manifest.bin).")
    parser.add_argument("--metadata-root", type=Path,
                        default=Path(__file__).resolve().parent.parent / "metadata",
                        help="Override the metadata search root.")
    args = parser.parse_args()

    project = _load_board_yaml(args.board_yaml)
    sku = (project.get("som") or {}).get("sku")
    if not sku:
        sys.exit("program_eeprom: board.yaml has no som.sku")
    family = _resolve_family(sku, args.metadata_root)
    hw_rev = _resolve_hw_rev(project, args.metadata_root)
    mfg = _parse_date(args.mfg_date)

    blob = _build_manifest(family, sku, hw_rev, args.serial, mfg)
    if len(blob) != MANIFEST_SIZE:
        sys.exit(f"program_eeprom: manifest is {len(blob)} bytes, "
                 f"expected {MANIFEST_SIZE} -- header layout bug")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    print(f"program_eeprom: wrote {len(blob)} bytes to {args.output}")
    print(f"  family   {family}")
    print(f"  sku      {sku}")
    print(f"  hw_rev   {hw_rev}")
    print(f"  serial   {args.serial}")
    print(f"  mfg_date {mfg.isoformat()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
