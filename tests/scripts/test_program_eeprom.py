# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/program_eeprom.py.

Locks the byte layout the programmer emits against drift -- the
C reader (`alp_hw_info_eeprom_t` in include/alp/hw_info.h) and
the Python writer have to agree byte-for-byte, so the size + the
field offsets + the CRC are pinned here.

Run:

    python -m unittest tests.scripts.test_program_eeprom
"""

from __future__ import annotations

import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from datetime import date
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "scripts" / "program_eeprom.py"


def _import_tool() -> object:
    """Import program_eeprom.py as a module so we can call its
    helpers directly without spawning subprocesses for every test."""
    spec = importlib.util.spec_from_file_location("program_eeprom", TOOL)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestManifestLayout(unittest.TestCase):

    def test_manifest_is_exactly_128_bytes(self) -> None:
        """The C reader asserts sizeof(alp_hw_info_eeprom_t) == 128
        with a _Static_assert; the Python writer must match."""
        tool = _import_tool()
        blob = tool._build_manifest(  # type: ignore[attr-defined]
            family="aen",
            sku="E1M-AEN701",
            hw_rev="r1",
            serial="2026W19-0001",
            mfg=date(2026, 5, 11),
        )
        self.assertEqual(len(blob), 128)

    def test_field_offsets_match_header(self) -> None:
        """Verify the magic + schema_version + string offsets land
        at the bytes the C struct expects (LE)."""
        tool = _import_tool()
        blob = tool._build_manifest(  # type: ignore[attr-defined]
            family="aen", sku="E1M-AEN701", hw_rev="r1",
            serial="serial-x", mfg=date(2026, 5, 11),
        )
        # Offsets mirror the field order in alp_hw_info_eeprom_t.
        self.assertEqual(struct.unpack_from("<I", blob, 0)[0], 0x414C5048)
        self.assertEqual(struct.unpack_from("<I", blob, 4)[0], 1)
        # family @ offset 8, 16 bytes, null-terminated.
        self.assertEqual(blob[8:8 + 4], b"aen\0")
        # sku @ offset 24, 24 bytes.
        self.assertEqual(blob[24:24 + 11], b"E1M-AEN701\0")
        # hw_rev @ offset 48, 8 bytes.
        self.assertEqual(blob[48:48 + 3], b"r1\0")
        # serial @ offset 56, 24 bytes.
        self.assertEqual(blob[56:56 + 9], b"serial-x\0")
        # mfg_year @ offset 80, mfg_month @ 82, mfg_day @ 83.
        self.assertEqual(struct.unpack_from("<H", blob, 80)[0], 2026)
        self.assertEqual(blob[82], 5)
        self.assertEqual(blob[83], 11)
        # CRC32 occupies the last 4 bytes (124..128).
        expected_crc = zlib.crc32(blob[:124]) & 0xFFFFFFFF
        self.assertEqual(struct.unpack_from("<I", blob, 124)[0], expected_crc)

    def test_string_overflow_rejects(self) -> None:
        tool = _import_tool()
        with self.assertRaises(SystemExit):
            tool._build_manifest(  # type: ignore[attr-defined]
                family="aen",
                sku="E1M-AEN701",
                hw_rev="r1",
                serial="x" * 100,    # exceeds the 24-byte budget
                mfg=date(2026, 5, 11),
            )

    def test_cli_roundtrip_against_example_board_yaml(self) -> None:
        """End-to-end: run the tool against the shipped example
        board.yaml and confirm the binary file is 128 bytes + the
        right magic."""
        with tempfile.TemporaryDirectory() as td:
            out = Path(td) / "eeprom.bin"
            rv = subprocess.run(
                [sys.executable, str(TOOL),
                 "--board-yaml", str(REPO / "examples" / "gpio-button-led" / "board.yaml"),
                 "--serial", "2026W19-0001",
                 "--mfg-date", "2026-05-11",
                 "--output", str(out)],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            self.assertEqual(out.stat().st_size, 128)
            data = out.read_bytes()
            self.assertEqual(struct.unpack_from("<I", data, 0)[0], 0x414C5048)
            self.assertEqual(struct.unpack_from("<I", data, 4)[0], 1)


if __name__ == "__main__":
    unittest.main()
