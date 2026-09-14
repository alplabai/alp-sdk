# SPDX-License-Identifier: Apache-2.0
"""`_resolve_flash_device()` must not place a runtime mount on a
`memory_map` region whose `write_authority` forbids it (alp-sdk#2088).

Mirrors `check_atoc_reservation._check_top_write_authority()`
(alp-sdk#2086): same vocabulary (`customer_runtime` is the only value a
runtime mount/carve-out may land on directly), opposite direction -- that
gate refuses a row that IS `customer_runtime` at the ATOC band; this
refuses a row that is NOT `customer_runtime` (and not a verifiably-safe
`composite` whole-device alias) anywhere it is named directly as a
`flash_device:`.

Two families of case here:

  - `TestRefusesWrongAuthority` / `TestAllowsCustomerRuntime` /
    `TestAbsentWriteAuthority` exercise `_resolve_flash_device()` directly
    against a hand-built, single-region `som_preset` --
    `resolve_memory_map()` returns a preset's own `memory_map:` verbatim
    (alp_project_loader.py:533-536) without touching `metadata_root`, so
    no real SoM YAML or SoC JSON needs to exist on disk for those cases.

  - `TestCompositeConsultsContainedRows` deepcopies the REAL, committed
    E1M-AEN801 preset and swaps only `memory_map:` -- the technique
    review round 1 on #2088 required: a single-region synthetic preset
    carries no `silicon:`/SoC data, so `resolve_aperture()` returns
    `None` and the composite-consult path this class exists to pin never
    even runs. `mram_main`'s `write_authority: composite` is the only
    whole-device alias any committed preset declares (every AEN SKU), so
    these cases are the literal #2088 hazard shape, not a contrived one.

Run locally:

    python -m pytest tests/scripts/test_orchestrate_storage_write_authority.py -v
"""

from __future__ import annotations

import copy
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import _write_board  # noqa: E402

from alp_orchestrate import load_board_yaml  # noqa: E402
from alp_orchestrate.aperture import resolve_aperture  # noqa: E402
from alp_orchestrate.paths import METADATA_ROOT  # noqa: E402
from alp_orchestrate.partition import (  # noqa: E402
    _composite_alias_coverage_gap,
    _resolve_flash_device,
)


def _preset(region: dict) -> dict:
    """A single-region preset with NO `silicon:` -- `resolve_aperture()`
    returns `None` for it, so this is the OFF-Alif shape (V2N/V2M/NX9101
    today): the deferred-to-required absent-write_authority leniency
    applies here, and the composite-consult completeness walk in
    `TestCompositeConsultsContainedRows` below never runs against it."""
    return {"sku": "TEST-SOM", "memory_map": [region]}


def _aen801_memory_map(tmp_path):
    """The real, committed E1M-AEN801 `memory_map:`, deepcopied so a test
    can freely mutate it without touching the loader's cached preset
    dict (shared across a process if any other test loaded the same
    SKU first)."""
    path = _write_board(tmp_path, """
    name: test-2088-aen801
    som:
      sku: E1M-AEN801
      hw_rev: r2
    cores:
      m55_hp:
        os: zephyr
        app: ./m55_hp
    storage: []
    """)
    project = load_board_yaml(path)
    return project.som_preset, copy.deepcopy(project.som_preset["memory_map"])


class TestRefusesWrongAuthority:
    def test_secure_enclave_row_is_refused(self):
        """The literal #2088 repro shape: a mount named directly at a
        row the Secure Enclave owns, not the application."""
        descriptor, reason = _resolve_flash_device(
            "atoc", _preset({
                "name": "atoc", "base": 0x80578000, "size_kib": 32,
                "write_authority": "secure_enclave",
            }),
            METADATA_ROOT)
        assert descriptor is None, descriptor
        assert "atoc" in reason
        assert "write_authority: 'secure_enclave'" in reason
        assert "is not customer-writable at runtime" in reason
        assert "write_authority: 'customer_runtime'" in reason, reason
        assert "#2088" in reason, reason

    def test_vendor_image_row_is_refused(self):
        """A factory-provisioned image (mcuboot's own tag) is equally
        off-limits -- the guard is not special-cased to 'atoc'."""
        descriptor, reason = _resolve_flash_device(
            "mcuboot", _preset({
                "name": "mcuboot", "base": 0x80000000, "size_kib": 64,
                "write_authority": "vendor_image",
            }),
            METADATA_ROOT)
        assert descriptor is None, descriptor
        assert "write_authority: 'vendor_image'" in reason, reason

    def test_none_authority_row_is_refused(self):
        """`none` ("nobody writes it") is an explicit no-writer, not an
        invitation to mount a filesystem there."""
        descriptor, reason = _resolve_flash_device(
            "reserved", _preset({
                "name": "reserved", "base": 0x80550000, "size_kib": 64,
                "write_authority": "none",
            }),
            METADATA_ROOT)
        assert descriptor is None, descriptor
        assert "write_authority: 'none'" in reason, reason


class TestAllowsCustomerRuntime:
    def test_customer_runtime_row_resolves(self):
        descriptor, reason = _resolve_flash_device(
            "storage", _preset({
                "name": "storage", "base": 0x80560000, "size_kib": 96,
                "write_authority": "customer_runtime",
            }),
            METADATA_ROOT)
        assert reason is None, reason
        assert descriptor is not None
        assert descriptor["name"] == "storage"


class TestAbsentWriteAuthority:
    """#2024: `write_authority` is deferred-to-required for som-preset v1.
    But ADR-0034 clause 4 and the schema both say absent means
    UNRESOLVED, never `customer_runtime` -- and `carveout.py`'s own
    equivalent guard already REFUSES this exact shape wherever an
    on-die MRAM aperture resolves (review round 1 on #2088: the
    original WARNING-only behaviour here was wrong, corrected to match).
    The rule is Alif-gated the same way `carveout.py`'s is: a no-op
    (silent, still legal) on a SoM that declares no aperture at all,
    since none of those presets author `write_authority` anywhere today
    and none could act on a refusal they have no `silicon:`-scoped
    aperture to justify."""

    def test_absent_write_authority_refuses_when_an_aperture_resolves(
            self, tmp_path):
        """The Alif case: an aperture resolves for this SoM (E1M-AEN801),
        so a region named directly as a `flash_device:` with no
        write_authority is refused, not merely warned about. The new row
        sits OUTSIDE the SoC's declared MRAM aperture (0x90000000, well
        past E8's window) so `_is_flash_sub_partition()` calls it a
        DEVICE, not a sub-partition -- every real `memory_map:` row that
        resolves as a device on AEN801 today is `mram_main` itself
        (`write_authority: composite`, covered by
        `TestCompositeConsultsContainedRows`), so isolating the
        directly-named absent-value path needs a second, out-of-aperture
        device; the SoM-level aperture still resolves regardless of
        where this one row's own extent lands, which is what the guard
        actually gates on (mirrors `carveout.py`'s own SoM-level gate)."""
        som_preset, memory_map = _aen801_memory_map(tmp_path)
        som_preset["memory_map"] = memory_map + [{
            "name": "sram_extra", "base": 0x90000000, "size_kib": 64,
            "accessible_from": ["m55_he", "m55_hp"], "cacheable": True,
            # write_authority deliberately omitted.
        }]
        descriptor, reason = _resolve_flash_device(
            "sram_extra", som_preset, METADATA_ROOT)
        assert descriptor is None, descriptor
        assert "no write_authority" in reason, reason
        assert "sram_extra" in reason, reason
        assert "ADR-0034" in reason, reason

    def test_absent_write_authority_stays_legal_with_no_aperture(self):
        """The non-Alif case (V2N/V2M/NX9101 today): no `silicon:` on
        this synthetic preset, so `resolve_aperture()` returns `None`
        and the absent-value refusal never applies -- mirrors
        `carveout.py`'s own `aperture is None` short-circuit."""
        descriptor, reason = _resolve_flash_device(
            "storage", _preset({
                "name": "storage", "base": 0x80560000, "size_kib": 96,
            }),
            METADATA_ROOT)
        assert reason is None, reason
        assert descriptor is not None
        assert descriptor["name"] == "storage"


class TestCompositeConsultsContainedRows:
    """`write_authority: composite` ("consult the contained rows
    instead", the schema's own words) must not be an unconditional pass
    -- review round 1 on #2088 reproduced three ways (below) that the
    original blanket exemption let a runtime mount land in the
    Secure-Enclave-owned `atoc` band. Every CASE here is the reviewer's
    own repro, ported verbatim onto the real E1M-AEN801 preset."""

    def test_unmodified_real_preset_still_resolves(self, tmp_path):
        """The control: the real, byte-for-byte committed E1M-AEN801
        `memory_map:` -- every row resolved, authorized, and fully
        tiling `mram_main`'s capacity -- must still resolve `mram_main`
        as a flash device (placement safety inside it, i.e. that every
        byte is ALSO reserved by name, is `_reserved_spans()`'s job and
        is covered by `test_fully_tiled_device_blocks_with_an_actionable_reason`
        in test_orchestrate_storage_region_bounds.py, not this guard's)."""
        som_preset, _ = _aen801_memory_map(tmp_path)
        descriptor, reason = _resolve_flash_device(
            "mram_main", som_preset, METADATA_ROOT)
        assert reason is None, reason
        assert descriptor is not None
        assert descriptor["name"] == "mram_main"

    def test_case_2_sibling_with_unresolved_base_is_refused(self, tmp_path):
        """CASE 2 (review round 1): `mram_main` resolves its own base
        (0x80000000, legal -- a SoM CAN author one), and `atoc` --
        the row that owns the address range `mram_main`'s own
        `size_kib` says it spans up to -- has an unresolved base. Before
        this fix, `_reserved_spans()`'s `self_region has a base` branch
        trusted `mram_main`'s own base directly and never verified the
        siblings tiled it, so a storage[] entry landed at 0x80578000 --
        inside the live SE ATOC band (`carveout.py:100-104`,
        bench-observed `ckBS` magic there)."""
        som_preset, memory_map = _aen801_memory_map(tmp_path)
        new_map = []
        for r in memory_map:
            if r["name"] == "mram_main":
                r = dict(r, base=0x80000000)
            elif r["name"] == "atoc":
                r = dict(r, base="TBD")
            new_map.append(r)
        som_preset["memory_map"] = new_map
        descriptor, reason = _resolve_flash_device(
            "mram_main", som_preset, METADATA_ROOT)
        assert descriptor is None, descriptor
        assert "atoc" in reason, reason
        assert "unresolved base" in reason, reason
        assert "#2088" in reason, reason

    def test_case_4_missing_sibling_leaves_an_unaccounted_gap(
            self, tmp_path):
        """CASE 4 (review round 1): `atoc` isn't declared AT ALL (not
        merely unresolved) -- the same live-ATOC-band hazard as CASE 2,
        reached a different way: nothing here can even name the row
        that's missing, only that `mram_main`'s declared capacity
        exceeds what its remaining siblings account for."""
        som_preset, memory_map = _aen801_memory_map(tmp_path)
        new_map = [dict(r, base=0x80000000) if r["name"] == "mram_main"
                   else r for r in memory_map if r["name"] != "atoc"]
        som_preset["memory_map"] = new_map
        descriptor, reason = _resolve_flash_device(
            "mram_main", som_preset, METADATA_ROOT)
        assert descriptor is None, descriptor
        assert "gap" in reason, reason
        assert "32" in reason, reason  # the missing 32 KiB atoc band.
        assert "#2088" in reason, reason

    def test_major1_overlap_plus_hole_of_equal_size_is_refused(
            self, tmp_path):
        """Review round 2, Major 1: delete `atoc` AND widen `he_slot0`
        by exactly `atoc`'s size (2688 -> 2720 KiB, +32 KiB). A SUM
        check alone passes this -- the missing 32 KiB from `atoc` is
        cancelled out by the 32 KiB `he_slot0` now double-claims from
        `hp_slot0` -- while `0x80578000..0x80580000`, the live SE ATOC
        band, stays completely undeclared and unreserved. Only a walk
        that requires actual CONTIGUITY (no overlap, no hole) catches
        this; a sum-of-sizes check cannot, by construction."""
        som_preset, memory_map = _aen801_memory_map(tmp_path)
        new_map = []
        for r in memory_map:
            if r["name"] == "mram_main":
                r = dict(r, base=0x80000000)
            elif r["name"] == "he_slot0":
                r = dict(r, size_kib=2720)
            if r["name"] == "atoc":
                continue
            new_map.append(r)
        som_preset["memory_map"] = new_map
        descriptor, reason = _resolve_flash_device(
            "mram_main", som_preset, METADATA_ROOT)
        assert descriptor is None, descriptor
        assert "overlaps" in reason, reason
        assert "he_slot0" in reason, reason
        assert "hp_slot0" in reason, reason
        assert "#2088" in reason, reason

    def test_case_5_own_base_unresolved_and_a_sibling_gap_is_refused(
            self, tmp_path):
        """CASE 5 (review round 1): `mram_main` carries its REAL shape
        (`base: "TBD"` -- every committed AEN preset ships this), and
        `atoc` is also unresolved. Pre-fix, `_reserved_spans()`'s
        min/max-derived identity check (`origin + capacity ==
        window_top`) failed here, and the code DEGRADED to sibling-only
        checking with a stderr warning -- not a refusal -- so a
        storage[] entry auto-allocated straight to offset 0 (0x80000000,
        MCUboot's own `vendor_image` band). This is the case review
        round 1 called out by name: an alias whose contained rows can't
        be evaluated must refuse, not resolve permissively."""
        som_preset, memory_map = _aen801_memory_map(tmp_path)
        new_map = [dict(r, base="TBD") if r["name"] == "atoc" else r
                   for r in memory_map]
        som_preset["memory_map"] = new_map
        descriptor, reason = _resolve_flash_device(
            "mram_main", som_preset, METADATA_ROOT)
        assert descriptor is None, descriptor
        assert "atoc" in reason, reason
        assert "#2088" in reason, reason

    def test_sibling_with_no_write_authority_is_refused_on_alif(
            self, tmp_path):
        """A sibling with a resolved base+size but NO write_authority is
        just as unverifiable as one with an unresolved base -- the
        aperture resolves for E1M-AEN801, so this is the same Alif-gated
        rule `TestAbsentWriteAuthority` pins for the directly-named
        case, applied to a row `mram_main` defers to instead."""
        som_preset, memory_map = _aen801_memory_map(tmp_path)
        new_map = []
        for r in memory_map:
            if r["name"] == "atoc":
                r = dict(r)
                del r["write_authority"]
            new_map.append(r)
        som_preset["memory_map"] = new_map
        descriptor, reason = _resolve_flash_device(
            "mram_main", som_preset, METADATA_ROOT)
        assert descriptor is None, descriptor
        assert "atoc" in reason, reason
        assert "no write_authority" in reason, reason

    def test_coverage_walk_itself_ignores_a_disjoint_device(self, tmp_path):
        """`_composite_alias_coverage_gap()` in isolation: a row outside
        `mram_main`'s window is not the alias's business -- containment,
        not co-listing, is what makes a row relevant to its own
        completeness check. Called directly (not through
        `_resolve_flash_device()`) to isolate this from Major 2's
        `_reserved_spans()` degradation below, which is a SEPARATE
        concern the coverage walk alone cannot see."""
        som_preset, memory_map = _aen801_memory_map(tmp_path)
        som_preset["memory_map"] = memory_map + [{
            "name": "unrelated_device", "base": 0x90000000, "size_kib": 64,
            "accessible_from": ["m55_he", "m55_hp"], "cacheable": True,
            "write_authority": "customer_runtime",
        }]
        gap_reason = _composite_alias_coverage_gap(
            "mram_main", "TBD", 5632 * 1024, som_preset, METADATA_ROOT,
            resolve_aperture(som_preset, METADATA_ROOT))
        assert gap_reason is None, gap_reason

    def test_reserved_spans_degradation_is_a_refusal_not_a_warning(
            self, tmp_path):
        """Review round 2, Major 2: `_reserved_spans()` is a SEPARATE,
        older function with its own weaker origin/window derivation --
        proving the contained rows individually safe
        (`_composite_alias_coverage_gap()`, pinned above) is not enough,
        because PLACEMENT routes through `_reserved_spans()`, not this
        guard. `mram_main`'s own base was `TBD` on every committed AEN
        preset when this test was written; #2053 (merged to dev) resolved
        it to `0x80000000`, so the fixture forces it back to `TBD` here to
        keep exercising the identity-derivation leg -- see
        `test_reserved_spans_does_not_degrade_when_the_alias_has_its_own_base`
        for the now-real resolved-base shape. With the base unresolved and
        an out-of-window sibling added, `_reserved_spans()`'s
        `window_top = max(hi for sized)` includes that sibling's
        `0x90010000`, failing its `origin + capacity == window_top`
        identity and degrading to `([], reason)` -- silently reserving
        NOTHING, not even `mcuboot` (`write_authority: vendor_image`), for
        whatever placement runs next. Pre-Major-2 this returned
        `status: ok` from `_resolve_flash_device()` because the coverage
        walk alone cannot see `_reserved_spans()`'s own derivation fail."""
        som_preset, memory_map = _aen801_memory_map(tmp_path)
        som_preset["memory_map"] = [
            dict(r, base="TBD") if r["name"] == "mram_main" else r
            for r in memory_map] + [{
                "name": "unrelated_device", "base": 0x90000000,
                "size_kib": 64, "accessible_from": ["m55_he", "m55_hp"],
                "cacheable": True, "write_authority": "customer_runtime",
            }]
        descriptor, reason = _resolve_flash_device(
            "mram_main", som_preset, METADATA_ROOT)
        assert descriptor is None, descriptor
        assert "can't be reserved" in reason, reason
        assert "#2088" in reason, reason

    def test_reserved_spans_does_not_degrade_when_the_alias_has_its_own_base(
            self, tmp_path):
        """The SAME out-of-window sibling does NOT degrade
        `_reserved_spans()` when `mram_main` carries its own resolved
        base: that branch (`self_region has a base`) uses the alias's
        OWN base as origin directly and never computes the fragile
        `window_top = max(hi for sized)` identity the unrelated sibling
        would otherwise poison. This is the shape
        test_orchestrate_storage_region_bounds.py's
        `test_a_named_alternative_with_a_real_dt_label_round_trips`
        fixture relies on (review round 2, Major 3 remedy)."""
        som_preset, memory_map = _aen801_memory_map(tmp_path)
        som_preset["memory_map"] = [
            dict(r, base=0x80000000) if r["name"] == "mram_main" else r
            for r in memory_map] + [{
                "name": "unrelated_device", "base": 0x90000000,
                "size_kib": 64, "accessible_from": ["m55_he", "m55_hp"],
                "cacheable": True, "write_authority": "customer_runtime",
            }]
        descriptor, reason = _resolve_flash_device(
            "mram_main", som_preset, METADATA_ROOT)
        assert reason is None, reason
        assert descriptor is not None
