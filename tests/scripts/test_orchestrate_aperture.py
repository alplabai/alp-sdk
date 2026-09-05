# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for `scripts/alp_orchestrate/aperture.py` -- SoC on-die flash
aperture resolution + region classification (#1365 split B).

Before this file, `aperture.py` (four public functions, ~180 lines) had zero
direct tests -- `grep -rn "alp_orchestrate.aperture\\|classify_region\\|
is_partition_inside_aperture\\|region_extent" tests/` returned nothing, even
though this module's own docstring states the governing doctrine an Opus
review later found two verdict-returning functions violating: containment is
ONE-SIDED -- inside the aperture proves flash, outside proves NOTHING.

Covers:
  - `resolve_aperture()` against the real E8 SoC spec (metadata/socs/alif/
    ensemble/e8.json: soc_flash_base 0x80000000, variant AE822FA0E5597LS0
    mram_mb 5.5 -> aperture [0x80000000, 0x80580000)), plus its None cases.
  - `region_extent()`, including the MAJOR-6 TBD-masking case
    (`{size_mib: "TBD", size_kib: 64}` must resolve via `size_kib`).
  - `classify_region()`'s four verdicts and every boundary: extent exactly
    equal to the aperture, straddling the low edge, straddling the high
    edge, fully outside, unresolved base, unresolved size -- and the
    MAJOR-5 fix itself: a DERIVED (non-preset-authored) row whose extent is
    contained in the aperture must classify `"flash"`, not `"ram"`.
  - `is_partition_inside_aperture()` returning `None` (not `False`) for a
    region outside the aperture (MAJOR 3) -- the fix that keeps
    `partition.py`'s `_is_flash_sub_partition()` falling back to the
    legacy `carveout:` flag instead of reading "not proven inside" as
    "is a flash device".

Run locally:

    python -m pytest tests/scripts/test_orchestrate_aperture.py -v
"""

from __future__ import annotations

from typing import Any, Optional

from alp_orchestrate.aperture import (
    classify_region,
    is_partition_inside_aperture,
    region_extent,
    resolve_aperture,
)
from alp_orchestrate.paths import METADATA_ROOT

# E8's real declared aperture (metadata/socs/alif/ensemble/e8.json):
# soc_flash_base 0x80000000, variant AE822FA0E5597LS0 (E1M-AEN801) declares
# mram_mb 5.5 -> top 0x80000000 + 5.5 MiB = 0x80580000.
APERTURE: tuple[int, int] = (0x80000000, 0x80580000)

_E8_PRESET: dict[str, Any] = {
    "sku": "E1M-TST001",
    "silicon": "alif:ensemble:e8",
    "silicon_variant": "AE822FA0E5597LS0",
}


def _region(base: Any, size_kib: Optional[int] = None,
            size_mib: Any = None, name: str = "r",
            **extra: Any) -> dict[str, Any]:
    region: dict[str, Any] = {"name": name, "base": base}
    if size_kib is not None:
        region["size_kib"] = size_kib
    if size_mib is not None:
        region["size_mib"] = size_mib
    region.update(extra)
    return region


class TestResolveAperture:
    def test_resolves_the_real_e8_aperture(self):
        assert resolve_aperture(_E8_PRESET, METADATA_ROOT) == APERTURE

    def test_none_when_preset_names_no_silicon(self):
        assert resolve_aperture({"sku": "no-silicon"}, METADATA_ROOT) is None

    def test_none_when_silicon_key_is_malformed(self):
        preset = {"silicon": "not-a-vendor-family-part"}
        assert resolve_aperture(preset, METADATA_ROOT) is None


class TestRegionExtent:
    def test_size_kib_field(self):
        r = _region(0x1000, size_kib=4)
        assert region_extent(r) == (0x1000, 0x1000 + 4 * 1024)

    def test_size_mib_field(self):
        r = _region(0x1000, size_mib=1)
        assert region_extent(r) == (0x1000, 0x1000 + 1024 * 1024)

    def test_unresolved_when_base_is_the_tbd_string(self):
        r = _region("TBD", size_kib=4)
        assert region_extent(r) is None

    def test_unresolved_when_base_is_absent(self):
        assert region_extent({"name": "r", "size_kib": 4}) is None

    def test_unresolved_when_neither_size_field_is_set(self):
        assert region_extent({"name": "r", "base": 0x1000}) is None

    def test_tbd_size_mib_does_not_mask_a_usable_size_kib(self):
        """MAJOR 6: the `memory_region` schema lets `size_kib` and
        `size_mib` each independently be `"TBD"`; a `"TBD"` in one field
        must never mask a usable integer sitting in the other."""
        r = _region(0x1000, size_kib=64, size_mib="TBD")
        assert region_extent(r) == (0x1000, 0x1000 + 64 * 1024)

    def test_tbd_size_kib_does_not_mask_a_usable_size_mib(self):
        r = _region(0x1000, size_mib=1, size_kib="TBD")
        assert region_extent(r) == (0x1000, 0x1000 + 1024 * 1024)


class TestClassifyRegion:
    def test_unresolved_when_region_base_is_unresolved(self):
        r = _region("TBD", size_kib=64)
        assert classify_region(r, APERTURE, True) == "unresolved"
        assert classify_region(r, APERTURE, False) == "unresolved"

    def test_unresolved_when_region_size_is_unresolved(self):
        r = {"name": "r", "base": APERTURE[0]}
        assert classify_region(r, APERTURE, True) == "unresolved"

    def test_unresolved_when_aperture_is_none(self):
        r = _region(APERTURE[0], size_kib=64)
        assert classify_region(r, None, True) == "unresolved"
        assert classify_region(r, None, False) == "unresolved"

    def test_flash_whole_device_alias_regardless_of_authorship(self):
        """Extent exactly equal to the aperture -- flash no matter who
        (nominally) authored the row."""
        r = _region(APERTURE[0], size_kib=5632)  # == [0x80000000, 0x80580000)
        assert region_extent(r) == APERTURE
        assert classify_region(r, APERTURE, True) == "flash"
        assert classify_region(r, APERTURE, False) == "flash"

    def test_flash_strictly_contained_even_when_not_preset_authored(self):
        """MAJOR 5: containment must be tested BEFORE `is_preset_authored`
        is consulted. A DERIVED row (is_preset_authored=False) whose
        extent sits inside the aperture is flash -- discarding that proof
        in favour of a blanket `"ram"` default is the exact inverse of
        the one-sidedness this module defends."""
        r = _region(APERTURE[0] + 0x10000, size_kib=64)  # strictly inside
        assert classify_region(r, APERTURE, False) == "flash"
        assert classify_region(r, APERTURE, True) == "flash"

    def test_ram_outside_the_aperture_and_not_preset_authored(self):
        r = _region(0xA0000000, size_kib=64)  # fully outside, e.g. OSPI XIP
        assert classify_region(r, APERTURE, False) == "ram"

    def test_unclassified_outside_the_aperture_and_preset_authored(self):
        r = _region(0xA0000000, size_kib=64)  # fully outside, e.g. OSPI XIP
        assert classify_region(r, APERTURE, True) == "unclassified"

    def test_straddling_the_low_edge_is_not_contained(self):
        # lo just below the floor, hi just above it: crosses the boundary,
        # neither fully inside nor fully outside.
        r = _region(APERTURE[0] - 0x1000, size_kib=8)
        assert classify_region(r, APERTURE, True) == "unclassified"
        assert classify_region(r, APERTURE, False) == "ram"

    def test_straddling_the_high_edge_is_not_contained(self):
        # lo just below the ceiling, hi just above it: crosses the boundary.
        r = _region(APERTURE[1] - 0x1000, size_kib=8)
        assert classify_region(r, APERTURE, True) == "unclassified"
        assert classify_region(r, APERTURE, False) == "ram"

    def test_tbd_size_mib_does_not_mask_size_kib_in_classification(self):
        """MAJOR 6, threaded through `classify_region` via `region_extent`."""
        r = _region(APERTURE[0] + 0x10000, size_kib=64, size_mib="TBD")
        assert classify_region(r, APERTURE, True) == "flash"


class TestIsPartitionInsideAperture:
    def test_true_for_a_proper_subset(self):
        r = _region(APERTURE[0] + 0x10000, size_kib=64)
        assert is_partition_inside_aperture(r, APERTURE) is True

    def test_false_only_when_extent_equals_the_aperture_exactly(self):
        r = _region(APERTURE[0], size_kib=5632)
        assert region_extent(r) == APERTURE
        assert is_partition_inside_aperture(r, APERTURE) is False

    def test_none_outside_the_aperture(self):
        """MAJOR 3: outside the aperture proves NOTHING, so the verdict is
        `None`, not `False`. Before this fix, `partition.py:46-50`'s
        `_is_flash_sub_partition()` read a definite `False` here as "not a
        sub-partition", i.e. "this is a flash DEVICE" -- exactly backwards
        for a row (e.g. an OSPI XIP window) that merely resolves outside
        the SoC's declared on-die MRAM aperture."""
        r = _region(0xA0000000, size_kib=64)
        assert is_partition_inside_aperture(r, APERTURE) is None

    def test_none_when_aperture_is_none(self):
        r = _region(APERTURE[0] + 0x10000, size_kib=64)
        assert is_partition_inside_aperture(r, None) is None

    def test_none_when_region_extent_is_unresolved(self):
        r = _region("TBD", size_kib=64)
        assert is_partition_inside_aperture(r, APERTURE) is None
