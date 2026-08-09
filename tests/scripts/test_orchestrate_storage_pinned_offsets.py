# SPDX-License-Identifier: Apache-2.0
"""
A pinned `storage[].offset_kib:` must not be dropped by an auto-allocated
sibling -- alplabai/tan-cli#554.

Storage entries are name-sorted, and the bump allocator only ever saw the
siblings placed BEFORE the entry it was placing. A pin whose name sorts late
therefore collided with a sibling the allocator had already put on top of it
and was refused -- `status: blocked` inside system-manifest.yaml, but silently
absent from the generated `dts-partitions.dtsi` with its filesystem Kconfig
(CONFIG_FILE_SYSTEM_LITTLEFS) never emitted, while the build reported success.
`docs/board-config-features.md`'s own `storage:` example is exactly that shape:
`pinned_low` (offset_kib: 0) sorts after `app_data` and `mcuboot_scratch`.

The fix places every pin FIRST, then bump-allocates around them.

Both headline tests were run against the PRE-fix resolver and failed there in
the wrong direction (measured output quoted per test).

E1M-AEN301 is used because its auto-derived memory_map is a bare `mram_main`
alias (5632 KiB) with no sibling regions to reserve, which isolates the
pinned-vs-bump interaction from the alp-sdk#1331 SoM-region bounds check
covered by test_orchestrate_storage_region_bounds.py.

Run locally:

    python -m pytest tests/scripts/test_orchestrate_storage_pinned_offsets.py -v
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


def _aen301(storage_body: str) -> str:
    return f"""
    name: test-aen301-pinned-offsets
    som:
      sku: E1M-AEN301
      hw_rev: r1

    cores:
      m55_hp:
        os: zephyr
        app: ./m55_hp

    storage:
    {storage_body}
    """


def _resolve(tmp_path, storage_body):
    path = _write_board(tmp_path, _aen301(storage_body))
    return resolve_storage_partitions(load_board_yaml(path))


def _by_name(parts):
    return {p.name: p for p in parts}


def _spans(parts):
    return sorted((p.base_kib, p.base_kib + p.size_kib, p.name)
                  for p in parts if p.status == "ok")


def _assert_disjoint(parts):
    spans = _spans(parts)
    for (lo_a, hi_a, name_a), (lo_b, hi_b, name_b) in zip(spans, spans[1:]):
        assert hi_a <= lo_b, (
            f"'{name_a}' {lo_a}..{hi_a} KiB overlaps '{name_b}' "
            f"{lo_b}..{hi_b} KiB")


class TestPinnedPartitionSurvivesTheBumpAllocator:
    def test_late_sorting_pin_is_not_dropped(self, tmp_path):
        """docs/board-config-features.md's own `storage:` example.

        Measured pre-fix:

            app_data        |status= ok      |base_kib= 0
            mcuboot_scratch |status= ok      |base_kib= 128
            pinned_low      |status= blocked |base_kib= 0
                            |reason= ... offset_kib=0 overlaps sibling
                                     partition 'app_data' on device 'mram_main'
            settings        |status= ok      |base_kib= 192

        `pinned_low` is the entry the customer explicitly pinned, and it is
        the one that disappeared from dts-partitions.dtsi.
        """
        parts = _resolve(tmp_path, """
      - { name: settings,        fs: littlefs, size_kib: 64,  mount: /lfs/settings, flash_device: mram_main }
      - { name: app_data,        fs: littlefs, size_kib: 128, mount: /lfs/app,      flash_device: mram_main }
      - { name: mcuboot_scratch, fs: raw,      size_kib: 32,                        flash_device: mram_main }
      - { name: pinned_low,      fs: raw,      size_kib: 32,  offset_kib: 0,        flash_device: mram_main }
    """)
        by_name = _by_name(parts)
        assert set(by_name) == {
            "settings", "app_data", "mcuboot_scratch", "pinned_low"}
        for name, part in by_name.items():
            assert part.status == "ok", f"{name}: {part.reason}"
        # The pin is honoured verbatim; everyone else allocates around it.
        assert by_name["pinned_low"].base_kib == 0
        assert by_name["app_data"].base_kib >= 32
        _assert_disjoint(parts)

    def test_early_sorting_pin_still_works(self, tmp_path):
        """The issue's minimal repro -- "pin the boot slot at 0, let the
        rest float".  Measured pre-fix on tan-cli's mirror:

            boot_slot |status= ok      |base_kib= 0
            cfg       |status= blocked |base_kib= 0
                      |reason= storage entry 'cfg' explicit offset_kib=None
                               overlaps sibling partition 'boot_slot' ...

        alp-sdk#1331's `_first_free` already fixed this direction here (and
        the "explicit offset_kib=None" misattribution with it); this test
        pins the behaviour so the two-pass rewrite cannot regress it.
        """
        parts = _by_name(_resolve(tmp_path, """
      - { name: boot_slot, offset_kib: 0, size_kib: 64, fs: raw,      flash_device: mram_main }
      - { name: cfg,                      size_kib: 64, fs: littlefs, flash_device: mram_main, mount: /cfg }
    """))
        assert parts["boot_slot"].status == "ok", parts["boot_slot"].reason
        assert parts["boot_slot"].base_kib == 0
        assert parts["cfg"].status == "ok", parts["cfg"].reason
        assert parts["cfg"].base_kib == 64

    def test_reason_never_calls_an_auto_offset_explicit(self, tmp_path):
        """The issue's secondary defect: the block reason interpolated the
        VICTIM's `entry.offset_kib`, so an auto-allocated entry was blamed
        for an "explicit offset_kib=None" it never declared.  No emitted
        reason may ever contain that string."""
        parts = _resolve(tmp_path, """
      - { name: aaa, offset_kib: 0,  size_kib: 64, fs: raw, flash_device: mram_main }
      - { name: bbb, offset_kib: 32, size_kib: 64, fs: raw, flash_device: mram_main }
      - { name: ccc,                 size_kib: 64, fs: raw, flash_device: mram_main }
    """)
        for part in parts:
            assert "offset_kib=None" not in (part.reason or ""), part.reason

    def test_two_pins_on_one_offset_still_block(self, tmp_path):
        """Placing pins first must not make them mutually invisible."""
        parts = _by_name(_resolve(tmp_path, """
      - { name: aaa, offset_kib: 0, size_kib: 64, fs: raw, flash_device: mram_main }
      - { name: bbb, offset_kib: 0, size_kib: 64, fs: raw, flash_device: mram_main }
    """))
        assert parts["aaa"].status == "ok", parts["aaa"].reason
        assert parts["bbb"].status == "blocked"
        assert "offset_kib=0 overlaps sibling partition 'aaa'" in (
            parts["bbb"].reason or "")

    def test_pure_auto_layout_is_unchanged(self, tmp_path):
        """No pins anywhere -- the shape every shipped board.yaml uses.
        Offsets must stay exactly where they were before the two-pass
        rewrite (name-sorted, bottom-up, 4 KiB pages)."""
        parts = _by_name(_resolve(tmp_path, """
      - { name: settings, fs: littlefs, size_kib: 64,  mount: /lfs/settings, flash_device: mram_main }
      - { name: app_data, fs: littlefs, size_kib: 128, mount: /lfs/app,      flash_device: mram_main }
    """))
        assert parts["app_data"].base_kib == 0
        assert parts["settings"].base_kib == 128
