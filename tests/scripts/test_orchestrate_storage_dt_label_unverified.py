# SPDX-License-Identifier: Apache-2.0
"""
A `storage[].flash_device:` sourced from a `memory_map:` region must never
reach `status: ok` on an unverified `dt_label` (alp-sdk#1556, follow-up to
alp-sdk#1484).

`_resolve_flash_device()` defaults `dt_label` to the device NAME whenever
the SoM preset sets no explicit `dt_label:` override, and that default was
never checked against the generated Zephyr board `.dts`. Measured against
the real repo before this fix:

    flash_device: ddr_main on a real, unmodified E1M-V2N101 project ->
        status: ok, dt_label: 'ddr_main'
    `grep -rn ddr_main zephyr/` -> no match anywhere in the generated tree

E1M-AEN801/301/401/601's own `mram_main` cannot show the SAME "ok" outcome
on a REAL preset any more: alp-sdk#1445 (landed after this defect, before
this fix) made every shipped AEN SoM's `mram_main` fully tiled by its own
`carveout: false` sub-regions (mcuboot/slot0/reserved/storage/atoc summing
to exactly its capacity), so a stock AEN board.yaml now blocks any
`mram_main`-targeting entry on overflow before ever reaching the dt_label
question. `test_mram_main_isolated_repro` below reproduces the issue's own
literal example anyway by hand-shrinking a real AEN preset's `memory_map:`
down to just the whole-window `mram_main` alias (the same
synthetic-preset technique `test_orchestrate_storage_region_bounds.py`
already uses) -- isolating the dt_label defect from the unrelated,
already-fixed-elsewhere full-tiling behaviour.

Run locally:

    python -m pytest tests/scripts/test_orchestrate_storage_dt_label_unverified.py -v
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import REPO, _write_board  # noqa: E402

from alp_orchestrate import (  # noqa: E402
    load_board_yaml,
    resolve_storage_partitions,
)
from alp_orchestrate.partition import _verified_alt_devices  # noqa: E402
from alp_orchestrate.paths import METADATA_ROOT  # noqa: E402


def _by_name(parts):
    return {p.name: p for p in parts}


def test_ddr_main_v2n101_real_preset_no_longer_resolves_ok(tmp_path: Path) -> None:
    """The issue's own repro shape, on a real, unmodified SoM preset.

    `ddr_main` has 4194176 KiB of free room on E1M-V2N101 (no `dt_label:`
    override anywhere in `metadata/`), so pre-fix this allocated cleanly at
    offset 0 and reported `status: ok` with a fabricated `dt_label` of
    'ddr_main' -- `grep -rn ddr_main zephyr/` finds no such node anywhere
    in the generated tree.
    """
    assert not any(
        "ddr_main" in p.read_text(encoding="utf-8")
        for p in (REPO / "zephyr").rglob("*.dts*")
    ), "fixture assumption broke: 'ddr_main' now appears in zephyr/"

    path = _write_board(tmp_path, """
    name: test-1556-ddr-main
    som:
      sku: E1M-V2N101
      hw_rev: r1

    cores:
      m33_sm:
        app: ./m33_sm

    storage:
      - { name: cfg, size_kib: 32, fs: raw, flash_device: ddr_main }
    """)
    project = load_board_yaml(path)
    parts = resolve_storage_partitions(project)
    cfg = _by_name(parts)["cfg"]
    assert cfg.status == "blocked", (
        f"'ddr_main' reached status: ok with a fabricated dt_label -- "
        f"the exact alp-sdk#1556 defect: {cfg}")
    reason = cfg.reason or ""
    assert "ddr_main" in reason, reason
    assert "unverified" in reason, reason
    assert "dt_label" in reason, reason


def test_mram_main_isolated_repro(tmp_path: Path) -> None:
    """The issue's literal example -- `flash_device: mram_main` on an AEN
    SoM -- isolated from alp-sdk#1445's (unrelated) full-tiling block by
    hand-shrinking the preset's `memory_map:` to just the whole-window
    `mram_main` alias.
    """
    assert not any(
        "mram_main" in p.read_text(encoding="utf-8")
        for p in (REPO / "zephyr").rglob("*.dts*")
    ), "fixture assumption broke: 'mram_main' now appears in zephyr/"

    path = _write_board(tmp_path, """
    name: test-1556-mram-main
    som:
      sku: E1M-AEN401
      hw_rev: r1

    cores:
      m55_hp:
        os: zephyr
        app: ./m55_hp

    storage:
      - { name: cfg, size_kib: 32, fs: raw, flash_device: mram_main }
    """)
    project = load_board_yaml(path)
    project.som_preset["memory_map"] = [
        r for r in project.som_preset["memory_map"]
        if r.get("name") == "mram_main"
    ]
    parts = resolve_storage_partitions(project)
    cfg = _by_name(parts)["cfg"]
    assert cfg.status == "blocked", (
        f"'mram_main' reached status: ok with a fabricated dt_label of "
        f"'mram_main' -- the exact alp-sdk#1556 defect: {cfg}")
    reason = cfg.reason or ""
    assert "mram_main" in reason, reason
    assert "unverified" in reason, reason
    # The generated board tree only defines `mram_storage`, never named
    # back as a false "remedy" -- naming it would be decorating a node
    # that ALSO wasn't verified through this same gate.
    assert "mram_storage" not in reason, reason


def test_remedy_never_names_an_unverified_alternative(tmp_path: Path) -> None:
    """`_verified_alt_devices()` is empty for every real SoM today (no
    `memory_map:` region anywhere in `metadata/` declares an explicit
    `dt_label:` override yet), so the block reason must say so plainly
    rather than recommend a device this resolver cannot verify -- the
    exact #1484 defect shape, one layer further in."""
    path = _write_board(tmp_path, """
    name: test-1556-no-alt
    som:
      sku: E1M-V2N101
      hw_rev: r1

    cores:
      m33_sm:
        app: ./m33_sm

    storage:
      - { name: cfg, size_kib: 32, fs: raw, flash_device: ddr_main }
    """)
    project = load_board_yaml(path)
    assert _verified_alt_devices(
        "ddr_main", project.som_preset, METADATA_ROOT) == []
    reason = _by_name(resolve_storage_partitions(project))["cfg"].reason or ""
    assert "no other flash_device:" in reason, reason
    assert "use a different flash_device:" not in reason, reason


def test_remedy_names_a_verified_alternative_when_one_exists(
        tmp_path: Path) -> None:
    """Synthetic drive for the round-trip: once a `memory_map:` region
    DOES carry an explicit, verified `dt_label:`, the remedy must name it
    -- proving the gate loosens correctly rather than staying permanently
    silent. E1M-AEN401 is used (not V2N101) because it declares an
    inline `memory_map:` list in its own preset YAML, so appending a
    synthetic region to `project.som_preset["memory_map"]` works the same
    way `test_orchestrate_storage_region_bounds.py`'s equivalent case
    does; V2N101's `memory_map:` is entirely SoC-JSON-derived (no
    `memory_map:` key on the preset dict at all)."""
    path = _write_board(tmp_path, """
    name: test-1556-with-alt
    som:
      sku: E1M-AEN401
      hw_rev: r1

    cores:
      m55_hp:
        os: zephyr
        app: ./m55_hp

    storage:
      - { name: cfg, size_kib: 32, fs: raw, flash_device: mram_main }
    """)
    project = load_board_yaml(path)
    project.som_preset["memory_map"] = [
        r for r in project.som_preset["memory_map"]
        if r.get("name") == "mram_main"
    ] + [{
        "name": "test_alt_device",
        "base": "TBD",
        "size_kib": 64,
        "accessible_from": ["m55_hp"],
        "cacheable": True,
        "dt_label": "test_alt_device",
    }]
    reason = _by_name(
        resolve_storage_partitions(project))["cfg"].reason or ""
    assert "use a different flash_device:" in reason, reason
    assert "test_alt_device" in reason, reason


def test_ospi_memories_device_is_unaffected(tmp_path: Path) -> None:
    """`on_module.ospi_memories:` devices (e.g. `ospi0`) are NOT gated by
    this fix -- that key's own dt_label verification is a separate,
    still-open gap (`_has_real_dt_label()`'s docstring), and this issue's
    repro is scoped to `memory_map:` regions only. A regression here would
    also break the existing storage test suite's device-independent
    allocator fixtures, which rely on `ospi0` resolving `status: ok`."""
    path = _write_board(tmp_path, """
    name: test-1556-ospi-unaffected
    som:
      sku: E1M-AEN301
      hw_rev: r1

    cores:
      m55_hp:
        os: zephyr
        app: ./m55_hp

    storage:
      - { name: cfg, size_kib: 32, fs: raw, flash_device: ospi0 }
    """)
    parts = resolve_storage_partitions(load_board_yaml(path))
    cfg = _by_name(parts)["cfg"]
    assert cfg.status == "ok", cfg.reason
    assert cfg.dt_label == "ospi0"
