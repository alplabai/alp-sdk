# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/program_eeprom_secure_page.py.

Locks the 64-byte Secure Data Page mirror byte layout the tool emits
against drift -- the C reader (`alp_secure_page_mirror_t` in
include/alp/hw_info.h) and the Python writer have to agree
byte-for-byte, so the size + the field offsets + the CRC are pinned
here, the same shape as tests/scripts/test_program_eeprom.py for the
128-byte manifest.

Run:

    python -m unittest tests.scripts.test_program_eeprom_secure_page
"""

from __future__ import annotations

import importlib.util
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from datetime import date
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "scripts" / "program_eeprom_secure_page.py"


def _import_tool() -> object:
    spec = importlib.util.spec_from_file_location("program_eeprom_secure_page", TOOL)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestSecurePageMirrorLayout(unittest.TestCase):

    def test_mirror_is_exactly_64_bytes(self) -> None:
        """The C reader asserts sizeof(alp_secure_page_mirror_t) == 64
        with a _Static_assert; the Python writer must match."""
        tool = _import_tool()
        blob = tool._build_mirror(  # type: ignore[attr-defined]
            sku="E1M-AEN801", hw_rev="2626-r2",
            serial="TEST-0001", mfg=date(2026, 5, 11),
        )
        self.assertEqual(len(blob), 64)

    def test_field_offsets_match_header(self) -> None:
        """Verify the magic + schema_version + string offsets land at
        the bytes alp_secure_page_mirror_t expects (LE) -- see this
        struct's byte table in EEPROM-MANIFEST-SPEC.md
        (alp-sdk-internal)."""
        tool = _import_tool()
        blob = tool._build_mirror(  # type: ignore[attr-defined]
            sku="E1M-AEN801", hw_rev="2626-r2",
            serial="TEST-0001", mfg=date(2026, 5, 11),
        )
        self.assertEqual(struct.unpack_from("<I", blob, 0x00)[0], 0x414C5350)
        self.assertEqual(blob[0x04], 1)
        # sku @ 0x05, 16 bytes.
        self.assertEqual(blob[0x05:0x05 + 11], b"E1M-AEN801\0")
        # hw_rev @ 0x15, 12 bytes.
        self.assertEqual(blob[0x15:0x15 + 8], b"2626-r2\0")
        # serial @ 0x21, 23 bytes.
        self.assertEqual(blob[0x21:0x21 + 10], b"TEST-0001\0")
        # mfg_year @ 0x38, mfg_month @ 0x3A, mfg_day @ 0x3B.
        self.assertEqual(struct.unpack_from("<H", blob, 0x38)[0], 2026)
        self.assertEqual(blob[0x3A], 5)
        self.assertEqual(blob[0x3B], 11)
        # crc32 occupies the last 4 bytes (0x3C..0x40).
        expected_crc = zlib.crc32(blob[:0x3C]) & 0xFFFFFFFF
        self.assertEqual(struct.unpack_from("<I", blob, 0x3C)[0], expected_crc)

    def test_matches_spec_worked_test_vector(self) -> None:
        """Byte-for-byte against EEPROM-MANIFEST-SPEC.md's worked test
        vector for a fictional E1M-AEN801 2626-r2 module, serial
        TEST-0001 (the same placeholder-labelled fictional serial the
        128-byte manifest's own test vector uses -- deliberately not a
        real allocated unit or a real SKU/serial pairing), mfg
        2026-05-11 -- CRC32 0xBABB16C3."""
        tool = _import_tool()
        blob = tool._build_mirror(  # type: ignore[attr-defined]
            sku="E1M-AEN801", hw_rev="2626-r2",
            serial="TEST-0001", mfg=date(2026, 5, 11),
        )
        self.assertEqual(
            blob.hex(),
            "50534c410145314d2d41454e383031000000000000323632362d72320000"
            "000000544553542d303030310000000000000000000000000000ea07050b"
            "c316bbba",
        )

    def test_string_overflow_rejects(self) -> None:
        tool = _import_tool()
        with self.assertRaises(SystemExit):
            tool._build_mirror(  # type: ignore[attr-defined]
                sku="E1M-AEN801", hw_rev="2626-r2",
                serial="x" * 100,    # exceeds the 23-byte budget
                mfg=date(2026, 9, 4),
            )

    def test_cli_roundtrip_against_example_board_yaml(self) -> None:
        """End-to-end: run the tool against the shipped example
        board.yaml and confirm the binary file is 64 bytes + the
        right magic."""
        with tempfile.TemporaryDirectory() as td:
            out = Path(td) / "secure-page.bin"
            rv = subprocess.run(
                [sys.executable, str(TOOL),
                 "--board-yaml", str(REPO / "examples" / "aen" / "aen-eeprom-provision" / "board.yaml"),
                 "--serial", "TEST-0001",
                 "--mfg-date", "2026-05-11",
                 "--output", str(out)],
                capture_output=True, text=True, encoding="utf-8",
                env={**os.environ, "PYTHONIOENCODING": "utf-8"}, check=False,
            )
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            self.assertEqual(out.stat().st_size, 64)
            data = out.read_bytes()
            self.assertEqual(struct.unpack_from("<I", data, 0)[0], 0x414C5350)
            self.assertEqual(data[4], 1)
            # hw_rev carries the composed board datecode, same as the
            # 128-byte manifest tool does for this same board.yaml.
            hw_rev = data[0x15:0x15 + 12].split(b"\0", 1)[0].decode("ascii")
            self.assertEqual(hw_rev, "2626-r2")

    def test_different_magic_from_array_manifest(self) -> None:
        """The mirror's magic must never collide with the array
        manifest's -- see the struct's doc comment for why."""
        secure_page_tool = _import_tool()
        spec = importlib.util.spec_from_file_location(
            "program_eeprom", REPO / "scripts" / "program_eeprom.py")
        assert spec is not None and spec.loader is not None
        manifest_tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(manifest_tool)
        self.assertNotEqual(secure_page_tool.MAGIC, manifest_tool.MAGIC)  # type: ignore[attr-defined]


if __name__ == "__main__":
    unittest.main()
