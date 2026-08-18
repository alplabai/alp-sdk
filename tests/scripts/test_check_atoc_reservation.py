#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_atoc_reservation.py (alp-sdk#1289).

Every case here was run against the PRE-fix tree first: the two "unreserved"
tests fail (gate returns 0, i.e. does not catch it) if the gate's top-region
logic is removed, and the real `origin/dev` board tree reproduces exactly the
`_top_partition_unreserved` shape below on all four committed AEN boards.
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
_SPEC = importlib.util.spec_from_file_location(
    "check_atoc_reservation", REPO / "scripts" / "check_atoc_reservation.py")
atoc = importlib.util.module_from_spec(_SPEC)
sys.modules["check_atoc_reservation"] = atoc
_SPEC.loader.exec_module(atoc)


def _dts(partitions: "list[tuple[str, int, int]]") -> str:
    """Render a minimal fixed-partitions .dts. (label, offset, size_kib)."""
    nodes = "\n".join(
        f"""			{label}_partition: partition@{off:x} {{
				label = "{label}";
				reg = <0x{off:x} DT_SIZE_K({kib})>;
			}};"""
        for label, off, kib in partitions)
    return f"""/dts-v1/;
&mram {{
	partitions {{
		compatible = "fixed-partitions";
{nodes}
	}};
}};
"""


_RESERVED = [("mcuboot", 0x000000, 64), ("storage", 0x560000, 96),
             ("atoc", 0x578000, 32)]
_UNRESERVED = [("mcuboot", 0x000000, 64), ("storage", 0x560000, 128)]


class TestDtsCheck(unittest.TestCase):
    def _write(self, name: str, partitions) -> Path:
        d = Path(self.tmp) / name
        d.mkdir(parents=True)
        p = d / f"{name}.dts"
        p.write_text(_dts(partitions), encoding="utf-8")
        return p

    def setUp(self):
        import tempfile
        self._tmpdir = tempfile.TemporaryDirectory()
        self.tmp = self._tmpdir.name
        self._orig_repo = atoc.REPO
        atoc.REPO = Path(self.tmp)

    def tearDown(self):
        atoc.REPO = self._orig_repo
        self._tmpdir.cleanup()

    def test_top_partition_reserved_passes(self):
        p = self._write("e1m_aen999_m55_hp", _RESERVED)
        self.assertEqual(atoc._check_dts(p), [])

    def test_top_partition_unreserved_fails(self):
        p = self._write("e1m_aen999_m55_hp", _UNRESERVED)
        failures = atoc._check_dts(p)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("'storage', not 'atoc'", failures[0])
        # The message must carry the actual window end, not a hardcoded one.
        self.assertIn("0x580000", failures[0])

    def test_a_dts_with_no_partition_table_is_not_an_error(self):
        d = Path(self.tmp) / "e1m_aen999_m55_hp"
        d.mkdir(parents=True)
        p = d / "x.dts"
        p.write_text("/dts-v1/;\n&uart0 { status = \"okay\"; };\n",
                     encoding="utf-8")
        self.assertEqual(atoc._check_dts(p), [])

    def test_window_top_is_derived_not_hardcoded(self):
        """A part with a SMALLER MRAM must still be policed at ITS top."""
        small = [("mcuboot", 0x000000, 64), ("storage", 0x160000, 128)]
        p = self._write("e1m_aen999_m55_hp", small)
        failures = atoc._check_dts(p)
        self.assertEqual(len(failures), 1, failures)
        # 0x160000 + 128 KiB = 0x180000 -- not the E8's 0x580000.
        self.assertIn("0x180000", failures[0])


class TestPresetCheck(unittest.TestCase):
    def setUp(self):
        import tempfile
        self._tmpdir = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmpdir.name)
        self._orig_repo = atoc.REPO
        atoc.REPO = self.tmp

    def tearDown(self):
        atoc.REPO = self._orig_repo
        self._tmpdir.cleanup()

    def _preset(self, body: str) -> Path:
        p = self.tmp / "E1M-TEST.yaml"
        p.write_text(body, encoding="utf-8")
        return p

    def test_reserved_top_region_passes(self):
        p = self._preset(
            "memory_map:\n"
            "  - { name: storage, base: 0x80560000, size_kib: 96 }\n"
            "  - { name: atoc,    base: 0x80578000, size_kib: 32 }\n")
        self.assertEqual(atoc._check_preset(p), [])

    def test_storage_at_the_top_fails(self):
        p = self._preset(
            "memory_map:\n"
            "  - { name: storage, base: 0x80560000, size_kib: 128 }\n")
        failures = atoc._check_preset(p)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("'storage'", failures[0])
        self.assertIn("0x80580000", failures[0])

    def test_tbd_base_regions_are_skipped(self):
        """`base: TBD` carries no address -- it must not crash or fire."""
        p = self._preset(
            "memory_map:\n"
            "  - { name: storage,   base: 0x80560000, size_kib: 96 }\n"
            "  - { name: atoc,      base: 0x80578000, size_kib: 32 }\n"
            "  - { name: mram_main, base: \"TBD\",      size_kib: 5632 }\n")
        self.assertEqual(atoc._check_preset(p), [])

    def test_whole_device_region_does_not_mask_the_top_owner(self):
        """A real-based whole-window region must not be accepted as the top.

        `mram_main` spans the entire window, so it shares the same top as
        `atoc`.  The SMALLEST region at that top is the one that owns it --
        otherwise a preset could hide `storage` under a whole-device alias.
        """
        p = self._preset(
            "memory_map:\n"
            "  - { name: mram_main, base: 0x80000000, size_kib: 5632 }\n"
            "  - { name: storage,   base: 0x80560000, size_kib: 128 }\n")
        failures = atoc._check_preset(p)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("'storage'", failures[0])

    def test_preset_with_no_memory_map_is_skipped(self):
        p = self._preset("som:\n  sku: E1M-TEST\n")
        self.assertEqual(atoc._check_preset(p), [])


class TestSlot0AddressCheck(unittest.TestCase):
    """alp-sdk#1482: zephyr,code-partition must match the preset's slot0."""

    def setUp(self):
        import tempfile
        self._tmpdir = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmpdir.name)
        self._orig_repo = atoc.REPO
        self._orig_presets = atoc.PRESETS
        atoc.REPO = self.tmp
        atoc.PRESETS = self.tmp / "metadata" / "e1m_modules"
        atoc.PRESETS.mkdir(parents=True)
        (atoc.PRESETS / "E1M-AEN999.yaml").write_text(
            "memory_map:\n"
            "  - { name: mcuboot,  base: 0x80000000, size_kib: 64 }\n"
            "  - { name: he_slot0, base: 0x80010000, size_kib: 2688, "
            "accessible_from: [m55_he] }\n"
            "  - { name: hp_slot0, base: 0x802b0000, size_kib: 2688, "
            "accessible_from: [m55_hp] }\n",
            encoding="utf-8")

    def tearDown(self):
        atoc.REPO = self._orig_repo
        atoc.PRESETS = self._orig_presets
        self._tmpdir.cleanup()

    def _board(self, name: str, slot0_off: int) -> Path:
        d = self.tmp / name
        d.mkdir(parents=True)
        p = d / f"{name}.dts"
        p.write_text(
            "/dts-v1/;\n"
            "/ { chosen { zephyr,code-partition = &slot0_partition; }; };\n"
            "&mram { partitions { compatible = \"fixed-partitions\";\n"
            f"\tslot0_partition: partition@{slot0_off:x} {{\n"
            f"\t\treg = <0x{slot0_off:x} DT_SIZE_K(2688)>;\n"
            "\t};\n"
            "}; };\n", encoding="utf-8")
        return p

    def test_matching_address_passes(self):
        p = self._board("e1m_aen999_m55_hp", 0x2b0000)
        self.assertEqual(atoc._check_slot0_address(p), [])

    def test_mismatched_address_fails(self):
        p = self._board("e1m_aen999_m55_hp", 0x10000)
        failures = atoc._check_slot0_address(p)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("0x80010000", failures[0])
        self.assertIn("0x802b0000", failures[0])

    def test_non_m55_board_dir_is_skipped(self):
        p = self._board("e1m_aen999_a32", 0x10000)
        self.assertEqual(atoc._check_slot0_address(p), [])

    def test_unknown_sku_is_skipped(self):
        p = self._board("e1m_aen000_m55_hp", 0x10000)
        self.assertEqual(atoc._check_slot0_address(p), [])

    def test_half_authored_preset_fails_clean_not_traceback(self):
        # he_slot0 declared, hp_slot0 missing -- the resolver raises
        # OrchestratorError; the gate must turn that into a failure
        # string, not propagate the exception (#1482 fix-round finding).
        (atoc.PRESETS / "E1M-AEN998.yaml").write_text(
            "memory_map:\n"
            "  - { name: mcuboot,  base: 0x80000000, size_kib: 64 }\n"
            "  - { name: he_slot0, base: 0x80010000, size_kib: 2688, "
            "accessible_from: [m55_he] }\n",
            encoding="utf-8")
        p = self._board("e1m_aen998_m55_hp", 0x2b0000)
        failures = atoc._check_slot0_address(p)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("hp", failures[0])


class TestRealTree(unittest.TestCase):
    def test_the_committed_tree_passes(self):
        self.assertEqual(atoc.main([]), 0)


if __name__ == "__main__":
    unittest.main()
