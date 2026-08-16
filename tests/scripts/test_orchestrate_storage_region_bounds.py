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

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import _write_board  # noqa: E402

from alp_orchestrate import (  # noqa: E402
    load_board_yaml,
    resolve_storage_partitions,
)


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
        """A block that doesn't say what to do instead is a dead end."""
        parts = _resolve(tmp_path, """
      - { name: logs, size_kib: 32, fs: littlefs, flash_device: mram_main, offset_kib: 0, mount: /lfs/logs }
    """)
        reason = _by_name(parts)["logs"].reason or ""
        assert "not customer-writable" in reason, reason
        assert "flash_device:" in reason, reason

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
    def test_the_customer_writable_region_still_works(self, tmp_path):
        """Naming `storage` directly is the documented remedy, so it must
        resolve -- otherwise the block message above sends people nowhere.

        Inside `storage` (96 KiB) there are no sibling SoM regions, so a
        partition allocates from its offset 0 as before.
        """
        parts = _resolve(tmp_path, """
      - { name: settings, size_kib: 32, fs: littlefs, flash_device: storage, mount: /lfs/settings }
    """)
        settings = _by_name(parts)["settings"]
        assert getattr(settings, "status", None) != "blocked", settings.reason
        assert settings.base_kib == 0, settings


class TestNoFalsePositives:
    def test_a_som_region_target_is_unaffected(self, tmp_path):
        """A partition aimed at a SoM region the map DOES leave free must
        still allocate -- the bounds check must not false-positive on it.

        This was `test_a_bare_alias_som_is_unaffected`, and it asserted the
        opposite target: E1M-AEN301 used to derive a bare `mram_main` alias
        with no sibling regions, so a partition could sit on the whole-MRAM
        overlay. alp-sdk#1445 gave AEN301 the same explicit partitioning
        E1M-AEN801 always had, and that layout fills the 5632 KiB device
        EXACTLY, so the overlay now has nothing free on any AEN SoM -- a
        partition targeting it is correctly blocked (E1M-AEN801 has always
        behaved this way; measured, both SKUs now agree).

        What the check must still never do is block a partition that fits a
        region the SoM genuinely reserves for it, which is what this now
        exercises against the 96 KiB `storage` region.
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
          - { name: settings, size_kib: 64, fs: littlefs, flash_device: storage, mount: /lfs/settings }
        """)
        parts = resolve_storage_partitions(load_board_yaml(path))
        settings = _by_name(parts)["settings"]
        assert getattr(settings, "status", None) != "blocked", settings.reason
        assert settings.base_kib == 0, settings
