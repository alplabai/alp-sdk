#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Storage-partition resolver -- board.yaml `storage:` entries -> resolved partitions.

Resolves each StorageEntry to a ResolvedPartition: picks the flash device (the
SoM memory_map region names + on_module.ospi_memories keys), computes base/size
with page alignment, and flags overlaps. An entry that can't resolve (unknown
device / no size) becomes a blocked ResolvedPartition. Extracted from
alp_orchestrate as the #285 partition seam.

Depends only downward -- models, paths (METADATA_ROOT), memregion (_PAGE /
_region_size_bytes), and alp_project.resolve_memory_map; nothing calls back into
the package __init__.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Optional

from alp_project import resolve_memory_map

from .memregion import _PAGE, _region_size_bytes
from .models import BoardProject, ResolvedPartition, StorageEntry
from .paths import METADATA_ROOT


def _known_flash_devices(
    som_preset: dict[str, Any],
    metadata_root: Path,
) -> list[str]:
    """Enumerate every flash-device name a `storage[].flash_device:` may
    reference for the given SoM preset.

    Today this is the union of:
      - `memory_map:` region names (explicit override or derived from
        the SoC variant), AND
      - `on_module.ospi_memories:` keys (when declared on the SoM).

    Kept as a list so the loader's "did you mean..." message can sort
    it deterministically.
    """
    names: set[str] = set()
    for region in resolve_memory_map(som_preset, metadata_root):
        n = region.get("name")
        if isinstance(n, str):
            names.add(n)
    om = som_preset.get("on_module") or {}
    ospi = om.get("ospi_memories") or {}
    if isinstance(ospi, dict):
        for k in ospi.keys():
            if isinstance(k, str):
                names.add(k)
    return sorted(names)


def _resolve_flash_device(
    flash_device: str,
    som_preset: dict[str, Any],
    metadata_root: Path,
) -> tuple[Optional[dict[str, Any]], Optional[str]]:
    """Resolve a board.yaml `flash_device:` reference to a device descriptor.

    Returns `(descriptor, None)` on success; `(None, reason)` when the
    device is known but cannot be allocated against (TBD capacity).
    The descriptor carries:
        name        -- the SDK device name (verbatim from board.yaml)
        dt_label    -- the Zephyr DT label to decorate
        size_bytes  -- capacity in bytes (None when unresolvable)

    Address allocation is offset-within-device; the descriptor does
    NOT carry a physical base because Zephyr's flash-mapping layer
    derives that from the DT controller node.  The emitted overlay
    references `&<dt_label>` and lets Zephyr handle the physical mapping.
    """
    # memory_map: region match first (covers the auto-derived MRAM /
    # SRAM region table from resolve_memory_map()).
    for region in resolve_memory_map(som_preset, metadata_root):
        if region.get("name") != flash_device:
            continue
        size_bytes = _region_size_bytes(region)
        if size_bytes is None:
            return None, (
                f"flash device '{flash_device}' resolves to memory_map "
                f"region but size_mib / size_kib is unset or TBD on "
                f"SoM {som_preset.get('sku', '<unknown>')}; the SoM "
                f"hasn't been HW-mapped yet")
        dt_label = (region.get("dt_label")
                    if isinstance(region.get("dt_label"), str)
                    else flash_device)
        return {
            "name":       flash_device,
            "dt_label":   dt_label,
            "size_bytes": size_bytes,
        }, None

    # on_module.ospi_memories: key match.
    om = som_preset.get("on_module") or {}
    ospi = om.get("ospi_memories") or {}
    if isinstance(ospi, dict) and flash_device in ospi:
        entry = ospi[flash_device] or {}
        cap = entry.get("capacity_mbit")
        if isinstance(cap, str) and cap.strip().upper() == "TBD":
            return None, (
                f"on_module.ospi_memories.{flash_device}.capacity_mbit "
                f"is TBD on SoM {som_preset.get('sku', '<unknown>')}; "
                f"fill the value or move the storage entry to a sized "
                f"flash device")
        if not isinstance(cap, int):
            return None, (
                f"on_module.ospi_memories.{flash_device}.capacity_mbit "
                f"is missing on SoM {som_preset.get('sku', '<unknown>')}; "
                f"the storage allocator needs an authoritative capacity")
        dt_label = (entry.get("dt_label")
                    if isinstance(entry.get("dt_label"), str)
                    else flash_device)
        # capacity_mbit is megabits; convert to bytes.
        size_bytes = int(cap) * 1024 * 1024 // 8
        return {
            "name":       flash_device,
            "dt_label":   dt_label,
            "size_bytes": size_bytes,
        }, None

    # Unknown device — the loader catches this earlier, but defend
    # in depth in case a resolver is called with a hand-built project.
    return None, (
        f"flash device '{flash_device}' is neither a memory_map region "
        f"nor an on_module.ospi_memories key on SoM "
        f"{som_preset.get('sku', '<unknown>')}")


def _blocked_partition(
    entry: StorageEntry,
    reason: str,
) -> ResolvedPartition:
    """Project a StorageEntry into a blocked ResolvedPartition."""
    return ResolvedPartition(
        name=entry.name,
        fs=entry.fs,
        flash_device=entry.flash_device or "",
        dt_label="",
        base_kib=0,
        size_kib=entry.size_kib,
        mount=entry.mount,
        status="blocked",
        reason=reason,
    )


def resolve_storage_partitions(
    project: BoardProject,
) -> list[ResolvedPartition]:
    """Allocate physical offsets for every storage[] entry.

    Algorithm (mirrors `resolve_carve_outs()`):
      1. Group entries by `flash_device:` (entries without one block
         immediately; the loader normally catches that, but this is
         the resolver's failure mode for hand-built projects).
      2. Within each group, sort by name for determinism (an explicit
         `offset_kib:` doesn't change sort order — it's an override
         the allocator simply respects).
      3. For each entry: if `offset_kib:` is set, honour it (page-
         aligned check; overlap check against prior entries).  Else
         allocate bottom-up from the current high-water mark, page-
         aligned.
      4. Block on capacity overflow, TBD device, page-misaligned
         offset, or sibling overlap.  Blocked entries land in the
         manifest with `status: blocked` + `reason:` so reviewers see
         the gap; the slice build trips when the DTS overlay is
         consumed.

    Spec note: byte-stable OTA images require the orchestrator to pin
    addresses (resolved in design Q1 for v0.6 -- option "Pin in
    orchestrator").  Customers get reproducible per-rebuild addresses
    by declaring stable `name:` slugs; partitions stay at the same
    offset across rebuilds as long as their relative order doesn't
    change.  Explicit `offset_kib:` is the escape hatch.
    """
    if not project.storage:
        return []

    # Group by flash device, preserving original order for the
    # downstream resolver (we re-sort for allocation determinism below).
    by_device: dict[str, list[StorageEntry]] = {}
    no_device: list[StorageEntry] = []
    for entry in project.storage:
        if not entry.flash_device:
            no_device.append(entry)
        else:
            by_device.setdefault(entry.flash_device, []).append(entry)

    resolved: list[ResolvedPartition] = []

    for entry in no_device:
        resolved.append(_blocked_partition(entry, (
            f"storage entry '{entry.name}' has no flash_device: "
            f"declared; add one referencing a SoM memory_map region "
            f"or an on_module.ospi_memories key")))

    # Iterate flash devices in alphabetical order; within each device,
    # entries are name-sorted for byte-stable allocation.
    for device_name in sorted(by_device.keys()):
        descriptor, block_reason = _resolve_flash_device(
            device_name, project.som_preset, METADATA_ROOT)
        if descriptor is None:
            for entry in by_device[device_name]:
                resolved.append(_blocked_partition(
                    entry, block_reason or "flash device unresolvable"))
            continue

        dt_label = descriptor["dt_label"]
        capacity_bytes = descriptor["size_bytes"]
        # Page-aligned high-water mark; sibling partitions allocate
        # bottom-up from offset 0.  Page = 4 KiB matches the IPC
        # carve-out allocator; storage erase blocks on every silicon
        # the SDK targets are 4 KiB-or-larger multiples.
        high_water_bytes = 0
        # Track allocated [lo, hi) ranges so explicit `offset_kib:`
        # overrides can be checked against sibling allocations even
        # when the bump allocator would have placed them differently.
        allocated: list[tuple[int, int, str]] = []   # (lo, hi, name)

        entries_sorted = sorted(
            by_device[device_name], key=lambda e: e.name)

        for entry in entries_sorted:
            size_bytes = entry.size_kib * 1024
            size_aligned = ((size_bytes + _PAGE - 1) // _PAGE) * _PAGE

            if entry.offset_kib is not None:
                base_bytes = entry.offset_kib * 1024
                if base_bytes % _PAGE != 0:
                    resolved.append(_blocked_partition(entry, (
                        f"storage entry '{entry.name}' explicit "
                        f"offset_kib={entry.offset_kib} is not page-"
                        f"aligned (4 KiB)")))
                    continue
            else:
                base_bytes = high_water_bytes

            top_bytes = base_bytes + size_aligned
            if top_bytes > capacity_bytes:
                resolved.append(_blocked_partition(entry, (
                    f"storage entry '{entry.name}' ({entry.size_kib} "
                    f"KiB at offset {base_bytes // 1024} KiB) overruns "
                    f"flash device '{device_name}' "
                    f"(capacity {capacity_bytes // 1024} KiB)")))
                continue

            # Sibling-overlap check (only meaningful when offset_kib was
            # supplied; the bump allocator can't overlap with itself).
            overlap_with: Optional[str] = None
            for lo, hi, peer_name in allocated:
                if not (top_bytes <= lo or base_bytes >= hi):
                    overlap_with = peer_name
                    break
            if overlap_with is not None:
                resolved.append(_blocked_partition(entry, (
                    f"storage entry '{entry.name}' explicit "
                    f"offset_kib={entry.offset_kib} overlaps sibling "
                    f"partition '{overlap_with}' on device "
                    f"'{device_name}'")))
                continue

            allocated.append((base_bytes, top_bytes, entry.name))
            if entry.offset_kib is None:
                # Only bump the allocator when we used it; explicit
                # offsets don't shift the high-water mark (they may be
                # below it deliberately, e.g. to reserve a low slot).
                high_water_bytes = top_bytes

            resolved.append(ResolvedPartition(
                name=entry.name,
                fs=entry.fs,
                flash_device=device_name,
                dt_label=dt_label,
                base_kib=base_bytes // 1024,
                size_kib=size_aligned // 1024,
                mount=entry.mount,
            ))

    return resolved
