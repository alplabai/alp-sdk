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
tiled `memory_map:` (#1069 disjoint slot0 + #1289 atoc). E1M-AEN301's map is
auto-derived to a bare `mram_main` alias with no sibling regions, which is why
the pre-existing storage tests are unaffected by this change -- there is
nothing there to reserve.

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

        # The remedy must name an ALTERNATIVE device (not `mram_main`
        # itself, which is what the entry already targeted and is fully
        # tiled) that actually resolves.
        known = _known_flash_devices(project.som_preset, METADATA_ROOT)
        alt = [d for d in known if d != "mram_main" and d in reason]
        assert alt, (
            f"reason names no alternative device from {known}: {reason}")
        for device in alt:
            descriptor, err = _resolve_flash_device(
                device, project.som_preset, METADATA_ROOT)
            assert descriptor is not None, (
                f"remedy named '{device}' but it does not resolve: {err}")

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
        `ospi0`, E1M-AEN301's real external OSPI NOR
        (`on_module.ospi_memories`, 32 MiB, unaffected by this fix).
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
