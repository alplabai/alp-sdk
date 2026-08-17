# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_orchestrate/ -- storage partition resolution
and emitters (resolve_storage_partitions(), emit_dts_partitions(),
emit_storage_mounts_c()), v0.6 schema-only gap closed.

Split out of the orchestrator test suite as part of issue #460 / #673
Phase 3 (module-size reduction).

Run locally:

    python -m pytest tests/scripts/test_orchestrate_storage.py -v
"""

from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path

import pytest
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import REPO, _write_board  # noqa: E402

from alp_orchestrate import (                       # noqa: E402
    OrchestratorError,
    _slice_alp_conf,
    emit_dts_partitions,
    emit_storage_mounts_c,
    emit_system_manifest,
    load_board_yaml,
    resolve_storage_partitions,
)


# ---------------------------------------------------------------------
# Storage partition resolver + emitters (v0.6 schema-only gap closed).
# ---------------------------------------------------------------------


# AEN301's `mram_main` overlay now fills exactly at the SoM's own
# partitioning (alp-sdk#1445: 64 + 2688 + 2688 + 64 + 96 + 32 = 5632 KiB),
# so it has no free space to bump-allocate customer storage into on ANY
# AEN SoM (E1M-AEN801 has always behaved this way -- #1445 just brought the
# other five SoMs into line). These tests are about allocator MECHANICS --
# bump allocation, pin ordering, emission shape -- not about the SoM-region
# bounds check (see test_orchestrate_storage_region_bounds.py for that), so
# they target `ospi0` instead: every AEN SoM declares a 256-Mbit (32768 KiB)
# NOR flash there with no sub-regions of its own, which keeps the sizes and
# offsets below exactly as they were and lets the bump allocator's own
# arithmetic stay the thing under test.
AEN_STORAGE = """
name: test-aen-storage
som:
  sku: E1M-AEN301
  hw_rev: r1

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp

storage:
  - { name: settings,        size_kib: 64,  fs: littlefs, flash_device: ospi0, mount: /lfs/settings }
  - { name: app_data,        size_kib: 128, fs: littlefs, flash_device: ospi0, mount: /lfs/app }
  - { name: mcuboot_scratch, size_kib: 32,  fs: raw,      flash_device: ospi0 }
"""


# Storage entry with an explicit offset override; combined with a
# bump-allocated sibling to exercise both code paths in one project.
AEN_STORAGE_OFFSET = """
name: test-aen-storage-offset
som:
  sku: E1M-AEN301

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp

storage:
  - { name: bump_alloc, size_kib: 64, fs: raw, flash_device: ospi0 }
  - { name: pinned_low, size_kib: 32, fs: raw, flash_device: ospi0, offset_kib: 4096 }
"""


def test_resolve_storage_partitions_happy(tmp_path: Path) -> None:
    """Three littlefs+raw partitions on ospi0 allocate bottom-up,
    page-aligned, name-sorted for determinism."""
    path = _write_board(tmp_path, AEN_STORAGE)
    project = load_board_yaml(path)
    parts = resolve_storage_partitions(project)
    assert len(parts) == 3
    # All three resolved OK against ospi0 (every AEN SoM's declared
    # 256-Mbit / 32768 KiB NOR flash).
    for p in parts:
        assert p.status == "ok", (p.name, p.reason)
        assert p.flash_device == "ospi0"
        assert p.dt_label == "ospi0"  # no dt_label override declared
    # Sorted alphabetically: app_data (0), mcuboot_scratch (128), settings (160).
    by_name = {p.name: p for p in parts}
    assert by_name["app_data"].base_kib == 0
    assert by_name["app_data"].size_kib == 128
    # Page-aligned: 128 KiB stays 128 KiB.
    assert by_name["mcuboot_scratch"].base_kib == 128
    assert by_name["mcuboot_scratch"].size_kib == 32
    assert by_name["settings"].base_kib == 160
    assert by_name["settings"].size_kib == 64


def test_resolve_storage_partitions_unknown_flash_device(
    tmp_path: Path,
) -> None:
    """A typoed flash_device: must surface as a loader error with the
    list of known devices in the message."""
    body = AEN_STORAGE.replace(
        "flash_device: ospi0, mount: /lfs/settings",
        "flash_device: not_a_real_region, mount: /lfs/settings",
        1,
    )
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError, match="not_a_real_region"):
        load_board_yaml(path)


def test_flash_device_resolves_against_explicit_metadata_root(
    tmp_path: Path,
) -> None:
    """#1485: `load_board_yaml(..., metadata_root=R)` must resolve
    `storage[].flash_device` against R, not the in-tree `metadata/` the
    module-global `METADATA_ROOT` pointed at pre-fix.

    E1M-V2N101 derives its memory_map from the SoC JSON (no inline
    override), so renaming a region there is visible through
    `resolve_memory_map()` -- the exact path `loader.py:1030`'s
    `_known_flash_devices()` call walks. A scratch root that renames
    `ocram_low` -> `ocram_scratch`: a board declaring the OLD (in-tree
    only) name must fail against the scratch root, and one declaring the
    NEW (scratch-only) name must pass -- pre-fix both false-passed and
    false-failed respectively because the check silently read the
    repo's own `metadata/` regardless of `metadata_root`.
    """
    meta = tmp_path / "metadata"
    shutil.copytree(REPO / "metadata", meta)
    n44 = meta / "socs" / "renesas" / "rzv2n" / "n44.json"
    spec = json.loads(n44.read_text(encoding="utf-8"))
    renamed = False
    for region in spec["memory_regions"]:
        if region.get("name") == "ocram_low":
            region["name"] = "ocram_scratch"
            renamed = True
    assert renamed, "n44.json no longer declares an 'ocram_low' region"
    n44.write_text(json.dumps(spec, indent=2) + "\n",
                    encoding="utf-8", newline="\n")

    def _v2n_board(flash_device: str) -> str:
        return f"""
        name: test-v2n-metadata-root
        som:
          sku: E1M-V2N101
          hw_rev: r1

        cores:
          m33_sm:
            os: zephyr
            app: ./m33

        storage:
          - {{ name: settings, size_kib: 64, fs: raw, flash_device: {flash_device} }}
        """

    # The scratch-only name resolves against the scratch root.
    scratch_path = _write_board(
        tmp_path, _v2n_board("ocram_scratch"), name="scratch.yaml")
    load_board_yaml(scratch_path, metadata_root=meta)

    # The in-tree-only name (pre-fix this silently passed, reading the
    # repo's own metadata/ instead of the scratch root) must now fail.
    stale_path = _write_board(
        tmp_path, _v2n_board("ocram_low"), name="stale.yaml")
    with pytest.raises(OrchestratorError, match="ocram_low"):
        load_board_yaml(stale_path, metadata_root=meta)

    # Unpatched: the in-tree name still resolves against the real
    # in-tree metadata/ (default path unaffected, as the issue notes).
    default_path = _write_board(
        tmp_path, _v2n_board("ocram_low"), name="default.yaml")
    load_board_yaml(default_path)


def test_resolve_storage_partitions_duplicate_name(
    tmp_path: Path,
) -> None:
    """Duplicate partition names within `storage:` must error eagerly."""
    body = """
    name: test-dup
    som: { sku: E1M-AEN301 }
    cores:
      m55_hp: { os: zephyr, app: ./m55_hp }
    storage:
      - { name: dup, size_kib: 64, fs: raw, flash_device: mram_main }
      - { name: dup, size_kib: 64, fs: raw, flash_device: mram_main }
    """
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError, match="more than once"):
        load_board_yaml(path)


def test_resolve_storage_partitions_explicit_offset(
    tmp_path: Path,
) -> None:
    """offset_kib: must be honoured verbatim and not shift the bump
    allocator (so byte-stable layouts stay byte-stable)."""
    path = _write_board(tmp_path, AEN_STORAGE_OFFSET)
    project = load_board_yaml(path)
    parts = resolve_storage_partitions(project)
    assert len(parts) == 2
    by_name = {p.name: p for p in parts}
    # Sort order is alphabetical: bump_alloc allocates first at 0;
    # pinned_low honours its explicit offset of 4096 KiB.
    assert by_name["bump_alloc"].status == "ok"
    assert by_name["bump_alloc"].base_kib == 0
    assert by_name["bump_alloc"].size_kib == 64
    assert by_name["pinned_low"].status == "ok"
    assert by_name["pinned_low"].base_kib == 4096
    assert by_name["pinned_low"].size_kib == 32


def test_resolve_storage_partitions_misaligned_offset(
    tmp_path: Path,
) -> None:
    """A page-misaligned offset_kib: must produce a `blocked` partition
    with a clear reason, not raise."""
    body = """
    name: test-misalign
    som: { sku: E1M-AEN301 }
    cores:
      m55_hp: { os: zephyr, app: ./m55_hp }
    storage:
      - { name: bad, size_kib: 64, fs: raw, flash_device: mram_main, offset_kib: 5 }
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    parts = resolve_storage_partitions(project)
    assert len(parts) == 1
    assert parts[0].status == "blocked"
    assert "page-aligned" in (parts[0].reason or "")


def test_resolve_storage_partitions_overflow(tmp_path: Path) -> None:
    """A partition larger than the device's capacity must block."""
    body = """
    name: test-overflow
    som: { sku: E1M-AEN301 }
    cores:
      m55_hp: { os: zephyr, app: ./m55_hp }
    storage:
      - { name: huge, size_kib: 1048576, fs: raw, flash_device: mram_main }
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    parts = resolve_storage_partitions(project)
    assert len(parts) == 1
    assert parts[0].status == "blocked"
    assert "overruns" in (parts[0].reason or "")


def test_resolve_storage_partitions_overlap(tmp_path: Path) -> None:
    """Two explicit offsets that overlap must surface the offending pair."""
    body = """
    name: test-overlap
    som: { sku: E1M-AEN301 }
    cores:
      m55_hp: { os: zephyr, app: ./m55_hp }
    storage:
      - { name: a, size_kib: 64, fs: raw, flash_device: ospi0, offset_kib: 0 }
      - { name: b, size_kib: 64, fs: raw, flash_device: ospi0, offset_kib: 32 }
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    parts = resolve_storage_partitions(project)
    # `a` allocates at offset 0 (0..64KiB), `b` overlaps via its
    # explicit offset 32 KiB; the resolver blocks `b` (alphabetically
    # later, processed after `a` is already in the allocated set).
    by_name = {p.name: p for p in parts}
    assert by_name["a"].status == "ok"
    assert by_name["b"].status == "blocked"
    assert "overlaps" in (by_name["b"].reason or "")


def test_resolve_storage_partitions_deterministic(tmp_path: Path) -> None:
    """Two identical projects must produce byte-identical resolutions."""
    p1 = _write_board(tmp_path, AEN_STORAGE, name="b1.yaml")
    p2 = _write_board(tmp_path, AEN_STORAGE, name="b2.yaml")
    r1 = resolve_storage_partitions(load_board_yaml(p1))
    r2 = resolve_storage_partitions(load_board_yaml(p2))
    assert [(p.name, p.base_kib, p.size_kib) for p in r1] == \
           [(p.name, p.base_kib, p.size_kib) for p in r2]


def test_emit_dts_partitions_shape(tmp_path: Path) -> None:
    path = _write_board(tmp_path, AEN_STORAGE)
    out = emit_dts_partitions(load_board_yaml(path))
    # Decorates the flash device's DT label.
    assert "&ospi0 {" in out
    # Standard fixed-partitions binding.
    assert 'compatible = "fixed-partitions";' in out
    assert "#address-cells = <1>;" in out
    assert "#size-cells = <1>;" in out
    # One partition node per entry, with label + reg.
    for name in ("settings", "app_data", "mcuboot_scratch"):
        assert f"{name}_partition: partition@" in out
        assert f'label = "{name}";' in out
    # Address determinism: app_data is at offset 0 bytes; settings is
    # at 160 KiB = 0x28000 bytes; size 64 KiB = 0x10000 bytes.
    assert "app_data_partition: partition@0 {" in out
    assert "settings_partition: partition@28000 {" in out
    assert "reg = <0x28000 0x10000>;" in out


def test_emit_dts_partitions_no_storage(tmp_path: Path) -> None:
    """A board with no `storage:` block must emit the stub-only file
    (so downstream `#include` still resolves) without raising."""
    body = """
    name: test-no-storage
    som: { sku: E1M-AEN301 }
    cores:
      m55_hp: { os: zephyr, app: ./m55_hp }
    """
    path = _write_board(tmp_path, body)
    out = emit_dts_partitions(load_board_yaml(path))
    assert "No `storage:` entries" in out
    assert "fixed-partitions" not in out


def test_emit_storage_mounts_c_littlefs(tmp_path: Path) -> None:
    """The optional C mount table must declare an fs_mount_t per
    mountable partition + the global alp_storage_mounts[] array."""
    path = _write_board(tmp_path, AEN_STORAGE)
    out = emit_storage_mounts_c(load_board_yaml(path))
    assert "/* clang-format off */" in out
    assert "/* clang-format on */" in out
    # littlefs declarations for the two littlefs-formatted partitions.
    assert "FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(alp_lfs_settings)" in out
    assert "FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(alp_lfs_app_data)" in out
    # raw partition has no mount: it is omitted from the table.
    assert "alp_mnt_mcuboot_scratch" not in out
    # The aggregate array carries the two mountable partitions.
    assert "alp_storage_mount_count = 2;" in out


def test_slice_alp_conf_storage_kconfig(tmp_path: Path) -> None:
    """The Kconfig fragment must enable CONFIG_FILE_SYSTEM_LITTLEFS for
    every littlefs entry, plus a documentation comment per littlefs
    partition so the customer knows how to reach the partition at
    runtime (FIXED_PARTITION_ID(<name>_partition)).  The previous
    `CONFIG_FS_LITTLEFS_PARTITION_<NAME>=y` per-partition emit was a
    fictional Kconfig -- modern Zephyr's per-partition LFS wiring is
    DT-driven (`fixed-partitions` node + chosen
    `zephyr,storage-partition`), and setting the undefined symbol
    aborted twister with "assignment to undefined symbol"."""
    path = _write_board(tmp_path, AEN_STORAGE)
    project = load_board_yaml(path)
    conf = _slice_alp_conf(project, project.cores["m55_hp"])
    assert "CONFIG_FILE_SYSTEM=y" in conf
    assert "CONFIG_FILE_SYSTEM_LITTLEFS=y" in conf
    # Per-partition hint comments (no live CONFIG_* line -- see comment block
    # above for why).
    assert "partition[settings]" in conf
    assert "FIXED_PARTITION_ID(settings_partition)" in conf
    assert "partition[app_data]" in conf
    assert "FIXED_PARTITION_ID(app_data_partition)" in conf
    # Raw partition does NOT get a littlefs comment.
    assert "partition[mcuboot_scratch]" not in conf
    # Live CONFIG_FS_LITTLEFS_PARTITION_* lines must never come back --
    # they are the undefined-symbol form that aborts the build.
    for stem in ("SETTINGS", "APP_DATA", "MCUBOOT_SCRATCH"):
        assert f"\nCONFIG_FS_LITTLEFS_PARTITION_{stem}=" not in conf
    # FAT / EXT2 not pulled in (no entries declared them).
    assert "CONFIG_FAT_FILESYSTEM_ELM" not in conf
    assert "CONFIG_FILE_SYSTEM_EXT2" not in conf


def test_resolve_storage_partitions_blocks_on_tbd_ospi(
    tmp_path: Path,
) -> None:
    """A partition pointing at an ospi_memories: entry with capacity_mbit
    TBD must block with a clear reason -- AEN301 declares ospi1 with
    capacity_mbit: TBD (ospi0 carries the known 256-Mbit NOR flash on
    CS0), so the storage emitter should not silently allocate against the
    unknown ospi1 capacity."""
    # AEN301 declares on_module.ospi_memories.ospi1 with capacity_mbit: TBD.
    body = """
    name: test-aen-ospi-tbd
    som: { sku: E1M-AEN301 }
    cores:
      m55_hp: { os: zephyr, app: ./m55_hp }
    storage:
      - { name: app_data, size_kib: 64, fs: littlefs, flash_device: ospi1 }
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    parts = resolve_storage_partitions(project)
    assert len(parts) == 1
    assert parts[0].status == "blocked"
    reason = parts[0].reason or ""
    assert "TBD" in reason
    assert "ospi1" in reason


def test_emit_system_manifest_carries_storage(tmp_path: Path) -> None:
    """`storage:` must round-trip through the system manifest so the
    flash-allocation map ships into build/system-manifest.yaml."""
    path = _write_board(tmp_path, AEN_STORAGE)
    project = load_board_yaml(path)
    out = emit_system_manifest(project)
    parsed = yaml.safe_load(out)
    assert "storage" in parsed
    names = sorted(p["name"] for p in parsed["storage"])
    assert names == ["app_data", "mcuboot_scratch", "settings"]
    # Each ok entry carries its resolved offset + size.
    for p in parsed["storage"]:
        assert "offset_kib" in p
        assert "size_kib" in p
        assert "dt_label" in p

