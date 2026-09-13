#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_atoc_reservation.py (alp-sdk#1289).

Every case here was run against the PRE-fix tree first: the two "unreserved"
tests fail (gate returns 0, i.e. does not catch it) if the gate's top-region
logic is removed, and the real `origin/dev` board tree reproduces exactly the
`_top_partition_unreserved` shape below on all four committed AEN boards.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
_SPEC = importlib.util.spec_from_file_location(
    "check_atoc_reservation", REPO / "scripts" / "check_atoc_reservation.py")
atoc = importlib.util.module_from_spec(_SPEC)
sys.modules["check_atoc_reservation"] = atoc
_SPEC.loader.exec_module(atoc)

# Reuse the E8 silicon header from test_check_atoc_aperture_tiling.py instead
# of a second copy that could drift from it (#2069) -- loaded under a name
# distinct from pytest's own collected module name for that file.
_TILING_SPEC = importlib.util.spec_from_file_location(
    "_atoc_aperture_tiling_header_src",
    REPO / "tests" / "scripts" / "test_check_atoc_aperture_tiling.py")
_tiling_mod = importlib.util.module_from_spec(_TILING_SPEC)
_TILING_SPEC.loader.exec_module(_tiling_mod)
_SILICON_HEADER = _tiling_mod._SILICON_HEADER

# The new (#2069) top-anchor rule's own two message shapes -- asserted
# verbatim below instead of a substring like '0x80580000' or "'storage'"
# that 4b/4c also happen to emit, so a test can't pass for the wrong
# reason (review round 1, finding 1/8).
_NOT_ATOC_MSG = "reaches the top of the SoC's declared MRAM aperture"
_NO_ROW_MSG = "no region in the declared memory_map ends at"

# A fully-tiled E8 aperture (mcuboot/he_slot0/hp_slot0/reserved/storage,
# summing to exactly 5632 KiB) with 'storage' -- not 'atoc' -- reaching
# the aperture top, and every row carrying `carveout: false`: this is
# the pre-#1289 hazard shape with NOTHING else for 4b (gaps/overlaps) or
# 4c (carveout disagreement) to say, so a failure against it can only be
# the new rule (review round 1, finding 1).
_TILED_STORAGE_AT_TOP = (
    "  - { name: mcuboot,  base: 0x80000000, size_kib: 64,   carveout: false }\n"
    "  - { name: he_slot0, base: 0x80010000, size_kib: 2688, carveout: false }\n"
    "  - { name: hp_slot0, base: 0x802b0000, size_kib: 2688, carveout: false }\n"
    "  - { name: reserved, base: 0x80550000, size_kib: 64,   carveout: false }\n"
    "  - { name: storage,  base: 0x80560000, size_kib: 128,  carveout: false }\n"
)

# Same shape as _TILED_STORAGE_AT_TOP but split into a `reserved` + 96 KiB
# `storage` + 32 KiB `atoc` tail (96 + 32 == 128, so the aperture math is
# unchanged) with every `write_authority` authored, atoc correctly at the
# top with a NON-runtime-writable authority (#2086): the safe baseline
# case (e) -- storage's own `customer_runtime` sits well below the top
# and must not trip the new guard.
_TILED_SAFE_ATOC_AT_TOP = (
    "  - { name: mcuboot,  base: 0x80000000, size_kib: 64,   carveout: false, "
    "write_authority: vendor_image }\n"
    "  - { name: he_slot0, base: 0x80010000, size_kib: 2688, carveout: false, "
    "write_authority: customer_image }\n"
    "  - { name: hp_slot0, base: 0x802b0000, size_kib: 2688, carveout: false, "
    "write_authority: customer_image }\n"
    "  - { name: reserved, base: 0x80550000, size_kib: 64,   carveout: false, "
    "write_authority: none }\n"
    "  - { name: storage,  base: 0x80560000, size_kib: 96,   carveout: false, "
    "write_authority: customer_runtime }\n"
    "  - { name: atoc,     base: 0x80578000, size_kib: 32,   carveout: false, "
    "write_authority: secure_enclave }\n"
)

# Same as _TILED_SAFE_ATOC_AT_TOP but the top row itself (`atoc`) is
# runtime-writable -- case (a): a CORRECTLY-NAMED top row is not enough.
_TILED_RUNTIME_ATOC_AT_TOP = _TILED_SAFE_ATOC_AT_TOP.replace(
    "write_authority: secure_enclave }\n", "write_authority: customer_runtime }\n")


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


class TestPresetCheckApertureTopRule(unittest.TestCase):
    """#2069: the top-anchor check must be scoped to the SoC's declared
    MRAM aperture, not the file-wide max of every authored row -- an
    OSPI0 HyperRAM/NOR row above the window must never win the "top of
    window" comparison just for being highest."""

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

    def test_storage_at_the_top_fails_aperture_resolving(self):
        """Aperture-resolving twin of test_storage_at_the_top_fails, on a
        FULLY TILED, all-`carveout: false` layout so 4b/4c stay silent and
        the single failure can only be the new rule (finding 1)."""
        p = self._preset(_SILICON_HEADER + "memory_map:\n" + _TILED_STORAGE_AT_TOP)
        failures = atoc._check_preset(p)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn(_NOT_ATOC_MSG, failures[0])
        self.assertIn("'storage'", failures[0])
        self.assertIn("0x80580000", failures[0])

    def test_storage_at_the_top_fails_with_hyperram_row_present(self):
        """KEY regression test: an out-of-aperture HyperRAM row must not
        make the gate skip the rule, and must add zero failures of its
        own -- the count stays 1, same as without the row."""
        p = self._preset(
            _SILICON_HEADER + "memory_map:\n" + _TILED_STORAGE_AT_TOP +
            "  - { name: hyperram, base: 0xA0000000, size_mib: 64 }\n")
        failures = atoc._check_preset(p)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn(_NOT_ATOC_MSG, failures[0])
        self.assertIn("'storage'", failures[0])
        self.assertIn("0x80580000", failures[0])

    def test_atoc_authored_outside_the_aperture_does_not_satisfy_the_rule(self):
        p = self._preset(
            _SILICON_HEADER + "memory_map:\n" + _TILED_STORAGE_AT_TOP +
            "  - { name: atoc, base: 0xA3FF8000, size_kib: 32 }\n")
        failures = atoc._check_preset(p)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn(_NOT_ATOC_MSG, failures[0])
        self.assertIn("'storage'", failures[0])
        self.assertIn("0x80580000", failures[0])

    def test_short_tiling_with_nothing_at_the_top_fails(self):
        p = self._preset(
            _SILICON_HEADER + "memory_map:\n"
            "  - { name: mcuboot, base: 0x80000000, size_kib: 64 }\n"
            "  - { name: storage, base: 0x80010000, size_kib: 5504 }\n")
        failures = atoc._check_preset(p)
        joined = "\n".join(failures)
        self.assertIn(_NO_ROW_MSG, joined)
        self.assertIn("0x80580000", joined)

    def test_atoc_straddling_the_top_fails(self):
        """atoc starts at the aperture top's natural offset but overruns
        it by 32 KiB, so no row ends exactly at full_hi -- the anchor
        point is left unowned, the same failure branch as a short tiling
        (finding 8: asserts the new rule's own message, not '0x80580000'
        alone, which 4b's own overflow message also carries)."""
        p = self._preset(
            _SILICON_HEADER + "memory_map:\n"
            "  - { name: mcuboot, base: 0x80000000, size_kib: 64 }\n"
            "  - { name: storage, base: 0x80010000, size_kib: 5536 }\n"
            "  - { name: atoc,    base: 0x80578000, size_kib: 64 }\n")
        failures = atoc._check_preset(p)
        joined = "\n".join(failures)
        self.assertIn(_NO_ROW_MSG, joined)
        self.assertIn("0x80580000", joined)

    def test_zero_size_row_at_the_top_is_not_treated_as_reserving_it(self):
        """finding 6: a zero-size `atoc` row AT the aperture top reserves
        nothing -- it must not satisfy the rule by merely existing at the
        right address. Without the `hi > lo` guard this preset would pass
        with zero failures, masking the hazard."""
        p = self._preset(
            _SILICON_HEADER + "memory_map:\n"
            "  - { name: mcuboot, base: 0x80000000, size_kib: 64 }\n"
            "  - { name: storage, base: 0x80010000, size_kib: 5504 }\n"
            "  - { name: atoc,    base: 0x80580000, size_kib: 0 }\n")
        failures = atoc._check_preset(p)
        joined = "\n".join(failures)
        self.assertIn(_NO_ROW_MSG, joined)
        self.assertIn("0x80580000", joined)

    def test_no_silicon_falls_back_to_file_wide_max_and_still_fails(self):
        """Pins the fail-closed fallback: no `silicon:` resolves no
        aperture, so today's file-wide-max behaviour applies VERBATIM --
        an out-of-window OSPI row still wins the top comparison and the
        gate still refuses it loudly, even with a correct 'atoc' band."""
        p = self._preset(
            "memory_map:\n"
            "  - { name: mcuboot,  base: 0x80000000, size_kib: 64 }\n"
            "  - { name: storage,  base: 0x80010000, size_kib: 5504 }\n"
            "  - { name: atoc,     base: 0x80570000, size_kib: 64 }\n"
            "  - { name: ospi_row, base: 0xA0000000, size_mib: 64 }\n")
        failures = atoc._check_preset(p)
        joined = "\n".join(failures)
        self.assertIn("'ospi_row'", joined)

    def test_whole_device_region_does_not_mask_the_top_owner_aperture_resolving(self):
        """Aperture-resolving twin of
        test_whole_device_region_does_not_mask_the_top_owner, on the same
        fully-tiled, all-`carveout: false` layout so the whole-device
        alias's own presence adds no 4b/4c noise (finding 1/8)."""
        p = self._preset(
            _SILICON_HEADER + "memory_map:\n"
            "  - { name: mram_main, base: 0x80000000, size_kib: 5632 }\n" +
            _TILED_STORAGE_AT_TOP)
        failures = atoc._check_preset(p)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn(_NOT_ATOC_MSG, failures[0])
        self.assertIn("'storage'", failures[0])

    def test_six_band_layout_with_hyperram_above_passes(self):
        p = self._preset(
            _SILICON_HEADER + "memory_map:\n"
            "  - { name: mcuboot,  base: 0x80000000, size_kib: 64,   carveout: false }\n"
            "  - { name: he_slot0, base: 0x80010000, size_kib: 2688, carveout: false }\n"
            "  - { name: hp_slot0, base: 0x802b0000, size_kib: 2688, carveout: false }\n"
            "  - { name: storage,  base: 0x80550000, size_kib: 96,   carveout: false }\n"
            "  - { name: scratch,  base: 0x80568000, size_kib: 64,   carveout: false }\n"
            "  - { name: atoc,     base: 0x80578000, size_kib: 32,   carveout: false }\n"
            "  - { name: hyperram, base: 0xa0000000, size_mib: 64 }\n")
        self.assertEqual(atoc._check_preset(p), [])

    def test_six_band_layout_with_hyperram_and_ospi0_nor_above_passes(self):
        """The top rule must be indifferent to what sits above the
        aperture -- adding a second out-of-window row changes nothing."""
        p = self._preset(
            _SILICON_HEADER + "memory_map:\n"
            "  - { name: mcuboot,   base: 0x80000000, size_kib: 64,   carveout: false }\n"
            "  - { name: he_slot0,  base: 0x80010000, size_kib: 2688, carveout: false }\n"
            "  - { name: hp_slot0,  base: 0x802b0000, size_kib: 2688, carveout: false }\n"
            "  - { name: storage,   base: 0x80550000, size_kib: 96,   carveout: false }\n"
            "  - { name: scratch,   base: 0x80568000, size_kib: 64,   carveout: false }\n"
            "  - { name: atoc,      base: 0x80578000, size_kib: 32,   carveout: false }\n"
            "  - { name: hyperram,  base: 0xa0000000, size_mib: 64 }\n"
            "  - { name: ospi0_nor, base: 0xa0000000, size_mib: 32 }\n")
        self.assertEqual(atoc._check_preset(p), [])


class TestPresetCheckTopRowWriteAuthority(unittest.TestCase):
    """#2086: the top-of-window rule must refuse ANY row ending at the
    declared window top that is runtime-writable, not just one
    mis-named -- ONE guard (`_check_top_write_authority`), called from
    both the aperture branch and the no-aperture fallback, since a
    whole-device alias's extent always reaches the same top a
    correctly-named 'atoc' row would. Split out from
    TestPresetCheckApertureTopRule because these cases exercise the
    write_authority guard, not the aperture-scoping fix #2069 covers."""

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

    def test_a_correctly_named_atoc_row_with_customer_runtime_fails(self):
        """Case (a): naming the top row 'atoc' is not enough. Fully
        tiled with every other row's authority non-runtime-writable, so
        the single failure can only be the new guard. The remedy must
        suggest 'secure_enclave' (the atoc band's own value), NOT
        'composite' -- 'composite' is reserved for a whole-device alias
        and is itself exempt, so suggesting it for a mis-authored 'atoc'
        row would let an author retag it and pass."""
        p = self._preset(_SILICON_HEADER + "memory_map:\n" + _TILED_RUNTIME_ATOC_AT_TOP)
        failures = atoc._check_preset(p)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("'atoc'", failures[0])
        self.assertIn("customer_runtime", failures[0])
        self.assertIn("secure_enclave", failures[0])
        self.assertNotIn("composite", failures[0])
        # Aperture resolved -- must claim the confirmed SETOOLS anchor.
        self.assertIn("SETOOLS top-anchors", failures[0])

    def test_b_whole_device_alias_with_customer_runtime_fails_aperture_resolving(self):
        """Case (b): a whole-device alias's extent always reaches the
        aperture top too, so the SAME guard catches it -- no separate
        4b/4c copy needed. Exactly one failure: the safe six-band layout
        underneath contributes none of its own (4b tiles exactly, 4c's
        rows all carry carveout: false). The remedy must suggest
        'composite' -- this row IS the whole-device alias."""
        p = self._preset(
            _SILICON_HEADER + "memory_map:\n"
            "  - { name: mram_alias, base: 0x80000000, size_kib: 5632, "
            "carveout: false, write_authority: customer_runtime }\n"
            + _TILED_SAFE_ATOC_AT_TOP)
        failures = atoc._check_preset(p)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("mram_alias", failures[0])
        self.assertIn("customer_runtime", failures[0])
        self.assertIn("composite", failures[0])
        self.assertIn("SETOOLS top-anchors", failures[0])

    def test_c_whole_device_alias_with_customer_runtime_fails_no_aperture_fallback(self):
        """Case (c): same as (b) but no `silicon:` resolves -- no
        aperture, so 4b/4c are skipped entirely (`_check_preset`'s own
        `if aperture is not None:` guard) and the ONLY thing that can
        fire is the no-aperture fallback branch's copy of this guard.
        Since no aperture resolved, the message must NOT claim a
        confirmed SETOOLS anchor point -- the file-wide max is a
        fail-closed stand-in only, not a proven anchor address."""
        p = self._preset(
            "memory_map:\n"
            "  - { name: mram_alias, base: 0x80000000, size_kib: 5632, "
            "carveout: false, write_authority: customer_runtime }\n"
            + _TILED_SAFE_ATOC_AT_TOP)
        failures = atoc._check_preset(p)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("mram_alias", failures[0])
        self.assertIn("customer_runtime", failures[0])
        self.assertIn("composite", failures[0])
        self.assertNotIn("SETOOLS top-anchors", failures[0])
        self.assertIn("fail-closed", failures[0])

    def test_d_composite_stays_exempt(self):
        """Case (d), value 1 of 3: `composite` -- mram_main's real
        value -- is the whole-device alias's OWN tag today and must stay
        exempt."""
        p = self._preset(
            _SILICON_HEADER + "memory_map:\n"
            "  - { name: mram_alias, base: 0x80000000, size_kib: 5632, "
            "carveout: false, write_authority: composite }\n"
            + _TILED_SAFE_ATOC_AT_TOP)
        self.assertEqual(atoc._check_preset(p), [])

    def test_d_none_stays_exempt(self):
        """Case (d), value 2 of 3: `none` (nobody writes it) stays exempt."""
        p = self._preset(
            _SILICON_HEADER + "memory_map:\n"
            "  - { name: mram_alias, base: 0x80000000, size_kib: 5632, "
            "carveout: false, write_authority: none }\n"
            + _TILED_SAFE_ATOC_AT_TOP)
        self.assertEqual(atoc._check_preset(p), [])

    def test_d_absent_authority_stays_exempt_but_is_skip_noted(self):
        """Case (d), value 3 of 3: an ABSENT `write_authority` is
        unresolved, never `customer_runtime` (ADR-0034 clause 4), so it
        must stay exempt too -- but the schema also says a consumer must
        treat it as ineligible for runtime write "and say so" (review
        round 1, finding 6): asserts the checker's existing SKIP-note
        channel actually fires, naming the row, not silence."""
        p = self._preset(
            _SILICON_HEADER + "memory_map:\n"
            "  - { name: mram_alias, base: 0x80000000, size_kib: 5632, "
            "carveout: false }\n"
            + _TILED_SAFE_ATOC_AT_TOP)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            failures = atoc._check_preset(p)
        self.assertEqual(failures, [])
        printed = buf.getvalue()
        self.assertIn("SKIP", printed)
        self.assertIn("mram_alias", printed)

    def test_e_runtime_writable_row_not_at_the_top_is_unaffected(self):
        """Case (e): `storage`'s own `write_authority: customer_runtime`
        in the safe baseline sits well below the declared top (`atoc`
        owns it, non-runtime-writable) -- the guard must not fire just
        because SOME row in the file is runtime-writable."""
        p = self._preset(_SILICON_HEADER + "memory_map:\n" + _TILED_SAFE_ATOC_AT_TOP)
        self.assertEqual(atoc._check_preset(p), [])

    def test_f_single_resolved_atoc_row_fallback_not_treated_as_alias(self):
        """No-aperture fallback, `floor`/`top` are the file-wide min/max
        over rows with a RESOLVED base -- if `atoc` is the only such row
        (e.g. `mram_main` is still `base: "TBD"`), its own extent
        trivially equals floor..top, so the exact-extent test alone
        would misidentify it as the whole-device alias and suggest
        `composite`. Retagging it `composite` would then pass the gate.
        The remedy must still say `secure_enclave`, named by 'atoc',
        because it IS named 'atoc' regardless of the extent match."""
        p = self._preset(
            "memory_map:\n"
            "  - { name: mram_main, base: \"TBD\", size_kib: 5632, "
            "write_authority: composite }\n"
            "  - { name: atoc, base: 0x80578000, size_kib: 32, "
            "write_authority: customer_runtime }\n")
        failures = atoc._check_preset(p)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("'atoc'", failures[0])
        self.assertIn("customer_runtime", failures[0])
        self.assertIn("secure_enclave", failures[0])
        self.assertNotIn("composite", failures[0])


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


class TestSlot0WindowCeilingCheck(unittest.TestCase):
    """alp-sdk#1981, review round-12b finding 3: the 4th `_check_preset`
    check -- no `aen_atoc.SLOT0_WINDOWS` ceiling may reach past this
    preset's own 'atoc' band base. Had no dedicated test; deleting the
    check left the suite green."""

    def setUp(self):
        import tempfile
        self._tmpdir = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmpdir.name)
        self._orig_repo = atoc.REPO
        atoc.REPO = self.tmp
        self._orig_windows = dict(atoc.aen_atoc.SLOT0_WINDOWS)

    def tearDown(self):
        atoc.REPO = self._orig_repo
        atoc.aen_atoc.SLOT0_WINDOWS = self._orig_windows
        self._tmpdir.cleanup()

    def _preset(self, body: str) -> Path:
        p = self.tmp / "E1M-TEST.yaml"
        p.write_text(body, encoding="utf-8")
        return p

    def test_ceiling_at_atoc_base_passes(self):
        atoc.aen_atoc.SLOT0_WINDOWS = {
            "A32_0": (0x80002000, 0x80578000 - 0x80002000)}  # ceiling == atoc base
        p = self._preset(
            "memory_map:\n"
            "  - { name: storage, base: 0x80560000, size_kib: 96 }\n"
            "  - { name: atoc,    base: 0x80578000, size_kib: 32 }\n")
        self.assertEqual(atoc._check_preset(p), [])

    def test_ceiling_past_atoc_base_fails(self):
        """The #1981-regression shape: a window ceiling one byte past the
        atoc band's own base (the raw-MRAM_END bug this check exists to
        catch)."""
        atoc.aen_atoc.SLOT0_WINDOWS = {
            "A32_0": (0x80002000, 0x80578001 - 0x80002000)}  # ceiling one byte past
        p = self._preset(
            "memory_map:\n"
            "  - { name: storage, base: 0x80560000, size_kib: 96 }\n"
            "  - { name: atoc,    base: 0x80578000, size_kib: 32 }\n")
        failures = atoc._check_preset(p)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("'A32_0'", failures[0])
        self.assertIn("0x80578001", failures[0])
        self.assertIn("0x80578000", failures[0])

    def test_no_atoc_region_does_not_crash_or_fire(self):
        """No 'atoc' region in this preset -- atoc_base is None, so the
        ceiling check must skip cleanly (the top-region check below it
        still fires on its own, unrelated grounds)."""
        atoc.aen_atoc.SLOT0_WINDOWS = {
            "A32_0": (0x80002000, 0x80580000 - 0x80002000)}
        p = self._preset(
            "memory_map:\n"
            "  - { name: storage, base: 0x80560000, size_kib: 128 }\n")
        failures = atoc._check_preset(p)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("reaches the top of the declared window", failures[0])


class TestRealTree(unittest.TestCase):
    def test_the_committed_tree_passes(self):
        self.assertEqual(atoc.main([]), 0)


if __name__ == "__main__":
    unittest.main()
