#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Storage-partition resolver -- board.yaml `storage:` entries -> resolved partitions.

Resolves each StorageEntry to a ResolvedPartition: picks the flash device (the
SoM memory_map region names + on_module.ospi_memories keys), computes base/size
with page alignment, and flags overlaps. An entry that can't resolve (unknown
device / no size) becomes a blocked ResolvedPartition. Extracted from
alp_orchestrate as the #285 partition seam.

Depends only downward -- models (dataclasses, incl. the project's resolved
metadata root), memregion (_PAGE / _region_size_bytes), and
alp_project.resolve_memory_map; nothing calls back into the package __init__.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any, Optional

from alp_project import resolve_memory_map
from sentinels import is_tbd

from .memregion import _PAGE, _region_size_bytes
from .models import BoardProject, ResolvedPartition, StorageEntry


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


def _reserved_spans(
    device_name: str,
    capacity_bytes: int,
    som_preset: dict[str, Any],
    metadata_root: Path,
) -> "tuple[list[tuple[int, int, str]], Optional[str]]":
    """Return the SoM's own regions as DEVICE-RELATIVE [lo, hi) spans.

    Returns `(spans, None)` on success, `([], reason)` when the spans can't
    be computed safely -- the caller then falls back to sibling-only overlap
    checking rather than trusting a guessed conversion (#1331).

    Why this is needed at all: `storage[].offset_kib:` (and the bump
    allocator) work in offsets WITHIN a flash device, while `memory_map:`
    regions declare ABSOLUTE bases. Nothing previously bridged the two, so
    every check was blind to the SoM's own layout -- on E1M-AEN801 that let a
    littlefs mount resolve to offset 0 of `mram_main`, which is MCUboot, with
    no `offset_kib:` involved and no `status: blocked`.

    The device base is DERIVED, then VERIFIED, never assumed:

      - If the device is itself a `memory_map:` region with an integer
        `base`, that base is the origin.
      - Otherwise (a whole-window alias like `mram_main`, which declares
        `base: TBD` deliberately) take the lowest integer base among the
        sibling regions and require `lowest + capacity == highest region
        top`. That identity is what proves the alias really spans the same
        window the fine-grained regions tile; if it does not hold, refuse to
        convert.

    `_resolve_flash_device`'s descriptor deliberately carries no physical
    base (Zephyr's flash-mapping layer derives it from the DT controller
    node), which is why the origin is reconstructed here instead of being
    read off the descriptor.
    """
    regions = [r for r in resolve_memory_map(som_preset, metadata_root)
               if isinstance(r, dict)]
    sized = [(r["base"], r["base"] + (_region_size_bytes(r) or 0), str(r.get("name")))
             for r in regions
             if isinstance(r.get("base"), int) and _region_size_bytes(r)]
    if not sized:
        return [], (f"SoM declares no addressed memory_map region, so "
                    f"'{device_name}' offsets cannot be checked against it")

    self_region = next((r for r in regions if r.get("name") == device_name), None)
    if self_region is not None and isinstance(self_region.get("base"), int):
        origin = self_region["base"]
    else:
        origin = min(lo for lo, _, _ in sized)
        window_top = max(hi for _, hi, _ in sized)
        if origin + capacity_bytes != window_top:
            return [], (
                f"flash device '{device_name}' has no declared base and its "
                f"capacity ({capacity_bytes} B) does not span the addressed "
                f"memory_map window (0x{origin:x}..0x{window_top:x}), so its "
                f"offsets cannot be mapped onto the SoM's regions")

    spans: "list[tuple[int, int, str]]" = []
    for lo, hi, name in sized:
        # The device itself is not an obstacle inside itself; nor is a region
        # that spans the whole window (a coarse alias) -- otherwise every
        # partition would "overlap" it and nothing could ever be placed.
        if name == device_name or (lo <= origin and hi - lo >= capacity_bytes):
            continue
        rel_lo, rel_hi = lo - origin, hi - origin
        # Clip to the device; a region outside it is simply not our problem.
        rel_lo, rel_hi = max(rel_lo, 0), min(rel_hi, capacity_bytes)
        if rel_hi > rel_lo:
            spans.append((rel_lo, rel_hi, name))
    return sorted(spans), None


def _first_free(
    base_bytes: int,
    size_aligned: int,
    allocated: "list[tuple[int, int, str]]",
    capacity_bytes: int,
) -> int:
    """Advance `base_bytes` past any span it collides with, page-aligned.

    The bump allocator must allocate AROUND the SoM's reserved regions, not
    merely notice a collision: refusing instead of skipping would break the
    ordinary case (a project that names no addresses at all) rather than fix
    it. Returns the first non-colliding page-aligned base, or a value past
    `capacity_bytes` when the device has no room left -- the caller reports
    that as a blocked entry.
    """
    moved = True
    while moved:
        moved = False
        for lo, hi, _ in sorted(allocated):
            if base_bytes < hi and lo < base_bytes + size_aligned:
                base_bytes = ((hi + _PAGE - 1) // _PAGE) * _PAGE
                moved = True
        if base_bytes >= capacity_bytes:
            return base_bytes
    return base_bytes


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
        if is_tbd(cap):
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
      3. Place every `offset_kib:`-pinned entry FIRST (page-aligned
         check; overlap check against the SoM's own regions and prior
         pins), then bump-allocate the rest bottom-up from the current
         high-water mark, stepping over the pins.  Pinning first is
         what keeps a pin that sorts LATE alphabetically from being
         dropped by a sibling the allocator had already placed on top
         of it (#554).
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
            device_name, project.som_preset, project.effective_metadata_root())
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

        # Seed with the SoM's OWN regions (#1331).  Without this the checks
        # below only ever saw sibling storage[] entries, so mcuboot, a core's
        # slot0 and the SE-owned atoc band were all invisible: on E1M-AEN801
        # a littlefs mount resolved to offset 0 of `mram_main` -- MCUboot --
        # with no offset_kib: declared and no status: blocked.
        reserved_spans, reserved_reason = _reserved_spans(
            device_name, capacity_bytes, project.som_preset,
            project.effective_metadata_root())
        reserved_names = {name for _, _, name in reserved_spans}
        allocated.extend(reserved_spans)
        if reserved_reason is not None:
            # Degrading to sibling-only checking is exactly the silent
            # false-PASS this fix exists to remove, so SAY so rather than
            # letting the caller believe the SoM's regions were honoured.
            print(f"alp_orchestrate.partition: WARNING: {reserved_reason}; "
                  f"storage offsets on '{device_name}' are checked against "
                  f"sibling partitions only", file=sys.stderr)

        entries_sorted = sorted(
            by_device[device_name], key=lambda e: e.name)

        # (base_bytes, size_aligned, reason) per entry INDEX.  Placement runs
        # in TWO passes over the name-sorted list -- every `offset_kib:` pin
        # first, then the bump allocator around them (#554).  In one pass a
        # pin only ever became an obstacle for the siblings that happened to
        # sort BEFORE it, so `pinned_low` (offset_kib: 0, the shape
        # docs/board-config-features.md itself documents) collided with an
        # already-auto-allocated `app_data` and was silently dropped from
        # dts-partitions.dtsi -- a validate-clean layout the allocator could
        # have satisfied.  Emission below still walks `entries_sorted`.
        # Keyed by index rather than name because board.schema.json does not
        # require `storage[].name` to be unique.
        placement: dict[int, tuple[int, int, Optional[str]]] = {}

        def _place(entry: StorageEntry) -> tuple[int, int, Optional[str]]:
            size_bytes = entry.size_kib * 1024
            size_aligned = ((size_bytes + _PAGE - 1) // _PAGE) * _PAGE

            if entry.offset_kib is not None:
                base_bytes = entry.offset_kib * 1024
                if base_bytes % _PAGE != 0:
                    return 0, size_aligned, (
                        f"storage entry '{entry.name}' explicit "
                        f"offset_kib={entry.offset_kib} is not page-"
                        f"aligned (4 KiB)")
            else:
                # Allocate AROUND the reserved regions and the pins, not
                # merely detect a collision with them (#1331 / #554):
                # refusing here instead of skipping would break every
                # project that names no addresses -- which is the common
                # case -- rather than fix it.
                base_bytes = _first_free(
                    high_water_bytes, size_aligned, allocated, capacity_bytes)

            top_bytes = base_bytes + size_aligned
            if top_bytes > capacity_bytes:
                free_note = ""
                if entry.offset_kib is None and reserved_spans:
                    free_note = (
                        f"; the SoM's own regions ("
                        f"{', '.join(sorted(reserved_names))}) already occupy "
                        f"{sum(hi - lo for lo, hi, _ in reserved_spans) // 1024} "
                        f"KiB of it")
                return 0, size_aligned, (
                    f"storage entry '{entry.name}' ({entry.size_kib} "
                    f"KiB at offset {base_bytes // 1024} KiB) overruns "
                    f"flash device '{device_name}' "
                    f"(capacity {capacity_bytes // 1024} KiB){free_note}")

            # Overlap check -- against sibling partitions AND the SoM's own
            # regions seeded above.
            overlap_with: Optional[str] = None
            for lo, hi, peer_name in allocated:
                if not (top_bytes <= lo or base_bytes >= hi):
                    overlap_with = peer_name
                    break
            if overlap_with is not None:
                where = (f"offset_kib={entry.offset_kib}"
                         if entry.offset_kib is not None
                         else f"auto-allocated offset {base_bytes // 1024} KiB")
                if overlap_with in reserved_names:
                    return 0, size_aligned, (
                        f"storage entry '{entry.name}' {where} overlaps SoM "
                        f"region '{overlap_with}' on device '{device_name}' "
                        f"-- that region is not customer-writable. Target it "
                        f"directly with flash_device: {overlap_with} if you "
                        f"meant to allocate inside it, or pick an offset "
                        f"outside the SoM's declared regions.")
                return 0, size_aligned, (
                    f"storage entry '{entry.name}' {where} overlaps "
                    f"sibling partition '{overlap_with}' on device "
                    f"'{device_name}'")

            allocated.append((base_bytes, top_bytes, entry.name))
            return base_bytes, size_aligned, None

        for index, entry in enumerate(entries_sorted):
            if entry.offset_kib is not None:
                placement[index] = _place(entry)
        for index, entry in enumerate(entries_sorted):
            if entry.offset_kib is None:
                base_bytes, size_aligned, reason = _place(entry)
                placement[index] = (base_bytes, size_aligned, reason)
                if reason is None:
                    # Only bump the allocator when we used it; explicit
                    # offsets don't shift the high-water mark (they may be
                    # below it deliberately, e.g. to reserve a low slot) --
                    # `allocated` is what makes the allocator step over them.
                    high_water_bytes = base_bytes + size_aligned

        for index, entry in enumerate(entries_sorted):
            base_bytes, size_aligned, reason = placement[index]
            if reason is not None:
                resolved.append(_blocked_partition(entry, reason))
                continue
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
