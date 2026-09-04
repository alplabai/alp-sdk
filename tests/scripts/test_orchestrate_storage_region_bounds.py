# SPDX-License-Identifier: Apache-2.0
"""
Storage offsets must be bounds-checked against the SoM's OWN memory_map
regions, not just against sibling storage[] entries (alp-sdk#1331).

Every test here was run against the PRE-fix resolver first. The two headline
cases (`test_auto_allocation_does_not_land_on_mcuboot`,
`test_explicit_offset_zero_is_refused`) PASS on the pre-fix code in the wrong
direction -- it happily resolved a littlefs mount onto MCUboot -- so they are
real regression coverage rather than tests that merely agree with the new
implementation.

E1M-AEN801 is used deliberately: it is the one SKU with an explicit, fully
tiled `memory_map:` (#1069 disjoint slot0 + #1289 atoc). E1M-AEN301 carries
the same explicit tiling since alp-sdk#1445 -- its `mram_main` is also fully
tiled by its own `carveout: false` sub-regions, with 0 KiB free -- so the
pre-existing storage tests that target E1M-AEN301 now target `ospi0`
instead, which `_resolve_flash_device()` still resolves (a
`capacity_mbit:` is declared in `metadata/e1m_modules/E1M-AEN301.yaml`'s
`on_module.ospi_memories`) with room to spare for the bump-allocator
bounds logic under test here. #1484 re-review: `ospi0` is NOT a verified
Devicetree label on E1M-AEN301 -- that SKU has no board tree at all under
`zephyr/boards/alp/` -- so do not read "resolves" as "is a real,
board-tree-backed flash device"; these tests exercise allocator bounds
logic, which is device-independent, not a claim that `ospi0` is wired up
on this SKU.

Run locally:

    python -m pytest tests/scripts/test_orchestrate_storage_region_bounds.py -v
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import _write_board  # noqa: E402

from alp_orchestrate import (  # noqa: E402
    OrchestratorError,
    load_board_yaml,
    resolve_storage_partitions,
)
from alp_orchestrate.partition import (  # noqa: E402
    _known_flash_devices,
    _resolve_flash_device,
)
from alp_orchestrate.paths import METADATA_ROOT  # noqa: E402


def _aen801(storage_body: str) -> str:
    return f"""
    name: test-aen801-bounds
    som:
      sku: E1M-AEN801
      hw_rev: r2

    cores:
      m55_hp:
        os: zephyr
        app: ./m55_hp

    storage:
    {storage_body}
    """


def _resolve(tmp_path, storage_body):
    path = _write_board(tmp_path, _aen801(storage_body))
    return resolve_storage_partitions(load_board_yaml(path))


def _by_name(parts):
    return {p.name: p for p in parts}


class TestAutoAllocation:
    def test_auto_allocation_does_not_land_on_mcuboot(self, tmp_path):
        """The DEFAULT path -- no offsets declared anywhere.

        Pre-fix this resolved to `offset_kib: 0` of `mram_main`, i.e. absolute
        0x80000000, which is the MCUboot partition: a littlefs mount on the
        bootloader, reported as success. It must never resolve to offset 0 of a
        whole-window device whose low bytes are MCUboot.
        """
        parts = _resolve(tmp_path, """
      - { name: app_data, size_kib: 64, fs: littlefs, flash_device: mram_main, mount: /lfs/app }
    """)
        app = _by_name(parts)["app_data"]
        if getattr(app, "status", None) == "blocked":
            # AEN801 is fully tiled, so "no room" is the correct answer.
            assert "mram_main" in (app.reason or "")
            return
        assert app.base_kib != 0, (
            "app_data resolved to offset 0 of mram_main -- that is MCUboot")

    def test_fully_tiled_device_blocks_with_an_actionable_reason(self, tmp_path):
        """AEN801's memory_map tiles all 5632 KiB, so mram_main has no room.

        Blocking is the truthful outcome; the reason must name the occupying
        regions rather than just saying the device is full.
        """
        parts = _resolve(tmp_path, """
      - { name: app_data, size_kib: 256, fs: littlefs, flash_device: mram_main, mount: /lfs/app }
    """)
        app = _by_name(parts)["app_data"]
        assert getattr(app, "status", None) == "blocked", app
        reason = app.reason or ""
        assert "mcuboot" in reason and "atoc" in reason, reason


class TestExplicitOffset:
    def test_explicit_offset_zero_is_refused(self, tmp_path):
        """`offset_kib: 0` on a whole-window device is MCUboot.

        Pre-fix: page-aligned, inside capacity, no siblings -> accepted.
        """
        parts = _resolve(tmp_path, """
      - { name: logs, size_kib: 64, fs: littlefs, flash_device: mram_main, offset_kib: 0, mount: /lfs/logs }
    """)
        logs = _by_name(parts)["logs"]
        assert getattr(logs, "status", None) == "blocked", logs
        assert "mcuboot" in (logs.reason or ""), logs.reason

    def test_the_refusal_names_the_remedy(self, tmp_path):
        """A block that doesn't say what to do instead is a dead end.

        The remedy must name a flash_device: that actually resolves
        (alp-sdk#1484) -- naming the reserved region itself back at the
        caller is a dead end, since that region is refused by
        `_resolve_flash_device()`. Prove it round-trips rather than just
        asserting the literal "flash_device:" substring, which the dead-end
        text also contained.
        """
        board_path = _write_board(tmp_path, _aen801("""
      - { name: logs, size_kib: 32, fs: littlefs, flash_device: mram_main, offset_kib: 0, mount: /lfs/logs }
    """))
        project = load_board_yaml(board_path)
        parts = resolve_storage_partitions(project)
        reason = _by_name(parts)["logs"].reason or ""
        assert "not customer-writable" in reason, reason

        # `mram_main` is fully tiled by its own carveout:false sub-regions
        # on every AEN preset (0 KiB free) -- the remedy must NOT lead with
        # "pick an offset on 'mram_main' outside the SoM's declared
        # regions", since that advice is unfollowable on the fully-tiled
        # case this test pins (AEN801 mram_main, 0 KiB free).
        assert "pick an offset on 'mram_main'" not in reason, reason
        assert "fully tiled" in reason, reason

        # #1484 re-review: AEN801's `ospi0` is a KNOWN, RESOLVING device
        # (`_known_flash_devices()` / `_resolve_flash_device()`) but it has
        # no verified Devicetree label (`_has_real_dt_label()` -- `ospi0`
        # is the OSPI CONTROLLER node in
        # zephyr/dts/alif/ensemble_e8_peripherals.dtsi, `status =
        # "disabled"`, no flash-chip child; neither E1M-AEN801 board `.dts`
        # enables it). Recommending it back to the customer would decorate
        # a node that isn't a working flash area -- the exact defect
        # #1484 is titled after -- so the remedy must name NO alternative
        # device at all until a real per-instance DT label is verified.
        known = _known_flash_devices(project.som_preset, METADATA_ROOT)
        alt = [d for d in known if d != "mram_main" and d in reason]
        assert not alt, (
            f"remedy names unverified alternative device(s) {alt}: {reason}")
        assert "use a different flash_device:" not in reason, reason
        assert "ospi0" not in reason, reason

        # A dead end must at least name where to follow up (#1556 re-review).
        assert "alp-sdk#1556" in reason, reason

    def test_offset_inside_the_atoc_band_is_refused(self, tmp_path):
        """The #1289 band, reached the customer-facing way.

        `storage` ends at 0x578000 = 5600 KiB; the atoc band is the 32 KiB
        above it. An explicit offset there must be refused by name.
        """
        parts = _resolve(tmp_path, """
      - { name: logs, size_kib: 16, fs: raw, flash_device: mram_main, offset_kib: 5600 }
    """)
        logs = _by_name(parts)["logs"]
        assert getattr(logs, "status", None) == "blocked", logs
        assert "atoc" in (logs.reason or ""), logs.reason


class TestReservedBytesLessThanCapacity:
    """The `reserved_bytes < capacity_bytes` branch (partition.py) -- the
    OTHER shape a reserved-region overlap can take, distinct from AEN's
    fully-tiled `mram_main`.  E1M-V2N101's `ddr_main` has a single reserved
    span (`m33_tcm`) and 4194176 KiB of free room, so this is the
    `pick an offset on '<device>' outside the SoM's declared regions`
    remedy, not the `fully tiled` one -- and per #1484 review it has NO
    prior coverage in this file (both TestExplicitOffset cases pin AEN801,
    which is fully tiled and never reaches this branch).
    """

    def test_v2n101_ddr_main_overlap_names_no_undefined_alternative(
            self, tmp_path):
        """Regression for the #1484 review major finding: naming `m33_tcm`
        (the very region just refused) or `ocram_low` as a "different
        flash_device:" alternative -- neither has a Devicetree label
        (`grep -rn "m33_tcm\\|ocram_low" zephyr/` finds no DT node for
        either), so following that remedy would decorate an undefined
        node. E1M-V2N101 has no `on_module.ospi_memories:` and no
        `memory_map:` region carries an explicit `dt_label:` override, so
        the correct remedy names NO alternative device at all.
        """
        path = _write_board(tmp_path, """
        name: test-v2n101-ddr-main-overlap
        som:
          sku: E1M-V2N101
          hw_rev: r1

        cores:
          m33_sm:
            app: ./m33_sm

        storage:
          - { name: blob, size_kib: 4096, fs: raw, flash_device: ddr_main, offset_kib: 917504 }
        """)
        project = load_board_yaml(path)
        parts = resolve_storage_partitions(project)
        blob = _by_name(parts)["blob"]
        assert getattr(blob, "status", None) == "blocked", blob
        reason = blob.reason or ""
        assert "m33_tcm" in reason, reason
        assert "not customer-writable" in reason, reason

        # The branch under test: room is free outside the reserved span,
        # so the remedy must lead with "pick an offset", not "fully tiled".
        assert "pick an offset on 'ddr_main'" in reason, reason
        assert "fully tiled" not in reason, reason

        # The defect: neither reserved region may be offered back as a
        # "different flash_device:" -- both are refused sub-regions with
        # no verified Devicetree label.
        assert "m33_tcm" not in reason.split(
            "not customer-writable")[1], (
            f"remedy re-offers the refused region: {reason}")
        assert "ocram_low" not in reason, reason
        assert "use a different flash_device:" not in reason, reason

    def test_a_named_alternative_always_round_trips(self, tmp_path):
        """Whenever the remedy DOES name an alternative (any SoM, any
        overlap), every name in the parenthesised list must itself
        resolve AND carry a verified Devicetree label -- guards against a
        future SoM/region reintroducing the #1484 defect shape.

        Kept general (not hardcoded to "no alternative is ever named")
        because `_has_real_dt_label()` legitimately starts returning
        `True` again once a `memory_map:` region grows an explicit
        `dt_label:`; this is deliberately NOT a tautology against that
        future -- it asserts the referent (`_has_real_dt_label()` +
        `_resolve_flash_device()` are exactly the two predicates
        `alt_devices` is filtered on, see partition.py), which is exactly
        what would need to move in lockstep if either predicate loosened
        incorrectly.
        """
        from alp_orchestrate.partition import _has_real_dt_label

        path = _write_board(tmp_path, _aen801("""
      - { name: logs, size_kib: 32, fs: littlefs, flash_device: mram_main, offset_kib: 0, mount: /lfs/logs }
    """))
        project = load_board_yaml(path)
        parts = resolve_storage_partitions(project)
        reason = _by_name(parts)["logs"].reason or ""
        if "use a different flash_device:" not in reason:
            return
        listed = reason.split(
            "use a different flash_device: (")[1].split(")")[0]
        for device in (d.strip() for d in listed.split(",")):
            assert _has_real_dt_label(
                device, project.som_preset, METADATA_ROOT), (
                f"remedy named '{device}' with no verified DT label")
            descriptor, err = _resolve_flash_device(
                device, project.som_preset, METADATA_ROOT)
            assert descriptor is not None, (
                f"remedy named '{device}' but it does not resolve: {err}")

    def test_a_named_alternative_with_a_real_dt_label_round_trips(
            self, tmp_path):
        """Synthetic drive for the round-trip guarantee above.

        `_has_real_dt_label()` returns False for every device on every SoM
        today (alp-sdk#1556), so `test_a_named_alternative_always_round_trips`
        never executes its own body -- its `if "use a different
        flash_device:" not in reason: return` guard fires every run. This
        pins the SAME property against a hand-built `som_preset` carrying an
        explicit `dt_label:` override, so the guarantee is actually
        exercised at least once rather than only ever agreeing with itself.
        """
        path = _write_board(tmp_path, _aen801("""
      - { name: logs, size_kib: 32, fs: littlefs, flash_device: mram_main, offset_kib: 0, mount: /lfs/logs }
    """))
        project = load_board_yaml(path)
        # A synthetic second device.  `base: "TBD"` keeps it out of
        # `_reserved_spans()`'s address-window derivation for `mram_main`
        # (only integer bases enter that computation), while it still
        # resolves via `_resolve_flash_device()` since `size_kib` is an int.
        project.som_preset["memory_map"] = list(
            project.som_preset["memory_map"]) + [{
                "name": "test_alt_device",
                "base": "TBD",
                "size_kib": 64,
                "accessible_from": ["m55_he", "m55_hp"],
                "cacheable": True,
                "dt_label": "test_alt_device",
            }]
        parts = resolve_storage_partitions(project)
        reason = _by_name(parts)["logs"].reason or ""
        assert "use a different flash_device:" in reason, reason
        assert "test_alt_device" in reason, reason

    def test_aen401_fully_tiled_names_no_undefined_alternative(
            self, tmp_path):
        """Regression for the #1484 re-review major finding: E1M-AEN401's
        `mram_main` is fully tiled (64 + 2688 + 2688 + 64 + 96 + 32 = 5632
        KiB, 0 KiB free -- `metadata/e1m_modules/E1M-AEN401.yaml:129-135`),
        and its `on_module.ospi_memories.ospi0` resolves
        (`_resolve_flash_device()`) but has NO verified Devicetree label:
        E1M-AEN401's board tree (`zephyr/boards/alp/e1m_aen401_m55_hp/`)
        never includes `zephyr/dts/alif/ensemble_e8_peripherals.dtsi`
        (the only file anywhere under `zephyr/` that defines an `ospi0:`
        node) and never declares any `ospi` node of its own
        (`grep -rn ospi zephyr/boards/alp/e1m_aen401_m55_hp/` finds none).
        Recommending `ospi0` here -- as the pre-re-review code did --
        decorated a Devicetree label the board tree never defines: the
        mirror of the E1M-V2N101 case above, on the SKU the original
        finding measured against.
        """
        path = _write_board(tmp_path, """
        name: test-aen401-fully-tiled
        som:
          sku: E1M-AEN401
          hw_rev: r1

        cores:
          m55_hp:
            os: zephyr
            app: ./m55_hp

        storage:
          - { name: logs, size_kib: 32, fs: littlefs, flash_device: mram_main, offset_kib: 0, mount: /lfs/logs }
        """)
        project = load_board_yaml(path)
        parts = resolve_storage_partitions(project)
        logs = _by_name(parts)["logs"]
        assert getattr(logs, "status", None) == "blocked", logs
        reason = logs.reason or ""
        assert "not customer-writable" in reason, reason
        assert "fully tiled" in reason, reason
        assert "use a different flash_device:" not in reason, reason
        assert "ospi0" not in reason, reason
        assert "alp-sdk#1556" in reason, reason


class TestTargetingARegionDirectly:
    def test_naming_the_mram_storage_subregion_directly_is_refused(
            self, tmp_path):
        """Naming `storage` directly used to be the documented remedy for
        the block above -- but `storage` is itself a `carveout: false`
        region, a partition label *inside* the `mram_storage` flash node,
        not a Devicetree label of its own (alp-sdk#1484). The loader must
        refuse it at load time with the same "Known devices" message a typo
        gets, not resolve it and decorate a label the board tree never
        defines.
        """
        with pytest.raises(
                OrchestratorError,
                match="does not resolve to any flash device"):
            _resolve(tmp_path, """
      - { name: settings, size_kib: 32, fs: littlefs, flash_device: storage, mount: /lfs/settings }
    """)

    def test_resolve_flash_device_refuses_the_subregion_directly(
            self, tmp_path):
        """Defense in depth: `_resolve_flash_device()` itself must refuse a
        `carveout: false` sub-region even for a caller that bypasses the
        loader's eager `_known_flash_devices()` cross-check (partition.py
        lines 193-206). Exercised directly since nothing else in this file
        reaches that branch -- the loader check above always fires first
        for a board.yaml-driven call.
        """
        path = _write_board(tmp_path, _aen801("""
      - { name: settings, size_kib: 32, fs: littlefs, flash_device: ospi0, mount: /lfs/settings }
    """))
        project = load_board_yaml(path)
        descriptor, reason = _resolve_flash_device(
            "storage", project.som_preset, METADATA_ROOT)
        assert descriptor is None, descriptor
        assert "is a partition inside a" in (reason or ""), reason
        assert "flash-class region" in (reason or ""), reason


class TestNoFalsePositives:
    def test_a_genuinely_free_flash_device_is_unaffected(self, tmp_path):
        """A partition aimed at a flash device the map DOES leave free must
        still allocate -- the bounds check must not false-positive on it.

        This was `test_a_bare_alias_som_is_unaffected`, then
        `test_a_som_region_target_is_unaffected` targeting the SoM's own
        `storage` region directly. alp-sdk#1484 removed `storage` as a legal
        `flash_device:` target -- it is a `carveout: false` region, a
        partition label *inside* the `mram_storage` flash node, not a
        Devicetree label of its own (see
        `test_naming_the_mram_storage_subregion_directly_is_refused` above).
        E1M-AEN301's `mram_main` is fully tiled by its own sub-regions
        (alp-sdk#1445), so no `memory_map:` device on this SoM is free; this
        now exercises the same "must not false-positive" property against
        `ospi0` (`on_module.ospi_memories`, 32 MiB via `capacity_mbit:
        256`), which `_resolve_flash_device()` still resolves and has room
        to spare -- unaffected by this fix, since the allocator bounds
        logic under test is device-independent. Not a claim that `ospi0`
        is a verified, board-tree-backed flash device on E1M-AEN301:
        #1484 re-review found that SKU has no board tree at all under
        `zephyr/boards/alp/`.
        """
        path = _write_board(tmp_path, """
        name: test-aen301-region-target
        som:
          sku: E1M-AEN301
          hw_rev: r1

        cores:
          m55_hp:
            os: zephyr
            app: ./m55_hp

        storage:
          - { name: settings, size_kib: 64, fs: littlefs, flash_device: ospi0, mount: /lfs/settings }
        """)
        parts = resolve_storage_partitions(load_board_yaml(path))
        settings = _by_name(parts)["settings"]
        assert getattr(settings, "status", None) != "blocked", settings.reason
        assert settings.base_kib == 0, settings
