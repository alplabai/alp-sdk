#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Carve-out resolver -- board.yaml `ipc:` entries -> resolved shared-memory regions.

Each IPC entry maps to a `ResolvedCarveOut`: the shared-memory base/size come
from the SoM's resolved memory_map, the region id is a deterministic FNV-1a
hash, plus the default mailbox channel and page alignment. An entry that can't
resolve (missing mailbox metadata / no memory_map) becomes a blocked
ResolvedCarveOut carrying the reason. Extracted from alp_orchestrate as the #285
carve-out seam.

Depends only downward -- models (dataclasses, incl. the project's resolved
metadata root), alp_project.resolve_memory_map and sentinels.is_tbd; nothing
calls back into the package __init__.
"""

from __future__ import annotations

from typing import Any, Optional

from alp_project import resolve_memory_map
from sentinels import is_tbd

from .models import BoardProject, IpcEntry, ResolvedCarveOut
from .memregion import _PAGE, _region_size_bytes



def _fnv1a_32(data: bytes) -> int:
    """FNV-1a 32-bit hash.  10 lines, no deps."""
    h = 0x811c9dc5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h



def _align_down(value: int, alignment: int) -> int:
    return value - (value % alignment)


def _resolve_default_mailbox_channel(
    mailbox: dict[str, Any],
    entry_name: str,
) -> int:
    """Pick the mailbox channel reserved for a given IPC name.

    Returns the channel `id` from mailbox.channels[] whose
    `reserved_for:` matches the entry name; falls back to channel 0
    when nothing matches (loader-rule check happens at emit time)."""
    for ch in mailbox.get("channels") or []:
        if ch.get("reserved_for") == entry_name:
            return int(ch["id"])
    return 0


def _blocked_carve_out(entry: IpcEntry, reason: str) -> ResolvedCarveOut:
    """Project an IpcEntry into a blocked ResolvedCarveOut.

    Used when SoM metadata isn't ready yet (TBD addresses, missing
    mailbox controller) or the board.yaml entry can't be satisfied
    (no region, collision, etc.).  The manifest records the entry as
    `status: blocked` + `reason: ...` so reviewers see the gap; the
    actual slice-build step is what trips on it.
    """
    return ResolvedCarveOut(
        name=entry.name,
        kind=entry.kind,
        endpoints=list(entry.endpoints),
        base=0, size=0, region="",
        cacheable=bool(entry.cacheable) if entry.cacheable is not None else False,
        src_ept=0, dst_ept=0, mailbox_channel=0,
        status="blocked", reason=reason,
    )


def resolve_carve_outs(
    project: BoardProject,
) -> list[ResolvedCarveOut]:
    """Spec §6.1 algorithm.

    1. Sort ipc entries alphabetically by name.
    2. For each entry, pick the first memory region whose
       accessible_from: covers every endpoint and whose `cacheable:`
       attribute matches the entry's preference (non-cacheable by
       default; explicit `cacheable: true` flips the preference).
    3. Emit a `status: blocked` entry when the matching region has a
       TBD base / size (the SoM isn't HW-mapped yet); the manifest
       records the block reason and the actual slice-build step is
       what fails.
    4. Allocate top-down within the region, page-aligned (4 KiB), and
       only inside the sub-extent every endpoint can actually address
       (`access_windows:`, #553).  An explicit `ipc[].address:` is
       placed FIRST, bounds-checked against that same window and
       overlap-checked against every carve-out already placed, so two
       channels can never resolve onto one physical address and an
       address in no region is refused instead of being labelled with
       one (#552).
    5. Endpoint IDs: FNV-1a of the entry name, low byte ORed with
       0x400 for src, +1 for dst.  Collision check at emit time.

    Phase 3 strict-channel-reservation enforcement (spec §6.4):
       - If the SoM preset's `mailbox.controller` is `TBD`, every
         rpmsg entry lands blocked with a hint pointing at the preset
         that owes the value.
       - If the controller is set but no channel is `reserved_for:
         alp_default_rpmsg` (and any `ipc[].kind == rpmsg` entry is
         present), the rpmsg entries land blocked so the customer
         adds an explicit reservation rather than silently dropping
         the channel to 0.
    """
    if not project.ipc:
        return []

    # Derive the effective memory-region table.  An explicit `memory_map:`
    # block in the SoM preset wins verbatim (non-stock partitioning); when
    # absent the helper derives the table from the SoC variant JSON so the
    # orchestrator doesn't need to duplicate that logic.
    memory_map = resolve_memory_map(
        project.som_preset, project.effective_metadata_root())
    mailbox = dict(project.som_preset.get("mailbox") or {})

    # Phase 3 strict mailbox checks (spec §6.4).  Surfaces preset
    # bugs before the user spends time on a build that would silently
    # collide on mailbox channel 0.  When metadata is incomplete, the
    # rpmsg entries land blocked rather than aborting resolution
    # (Phase 4 acceptance §6.1: emit a manifest, fail the build).
    has_rpmsg_entry = any(e.kind == "rpmsg" for e in project.ipc)
    rpmsg_block_reason: Optional[str] = None
    if has_rpmsg_entry:
        controller = mailbox.get("controller")
        if controller is None or is_tbd(controller):
            rpmsg_block_reason = (
                f"SoM {project.sku} mailbox controller is "
                f"{'unset' if controller is None else 'TBD'}; "
                f"carve-out resolution requires authoritative mailbox "
                f"metadata.  Fill `mailbox.controller:` in "
                f"metadata/e1m_modules/{project.sku}.yaml with the "
                f"vendor mailbox node name (e.g. `renesas_mhu`, "
                f"`nxp_mu`, `alif_mhuv2`) or remove the rpmsg "
                f"entries from board.yaml.")
        else:
            reserved_tags = {
                ch.get("reserved_for")
                for ch in (mailbox.get("channels") or [])
            }
            if "alp_default_rpmsg" not in reserved_tags:
                rpmsg_block_reason = (
                    f"no mailbox channel reserved for alp_default_rpmsg "
                    f"in {project.sku}; add one with `reserved_for: "
                    f"alp_default_rpmsg` to metadata/e1m_modules/"
                    f"{project.sku}.yaml mailbox.channels (e.g. "
                    f"`- {{ id: 0, reserved_for: alp_default_rpmsg }}`).")

    # Per-region high-water-mark allocator state.  Initialised lazily
    # the first time we touch a region; returns None when the region
    # carries a TBD base or unresolvable size.
    region_top: dict[str, int] = {}
    # Every span already handed out in a region, INCLUDING the ones
    # pinned by an explicit `ipc[].address:`.  Without this two carve-outs
    # resolve onto one physical window and both report `ok` (#552).
    placed_spans: dict[str, list[tuple[int, int, str]]] = {}

    def _region_top_init(region: dict[str, Any]) -> tuple[Optional[int], Optional[str]]:
        name = region["name"]
        if name in region_top:
            return region_top[name], None
        # A region derived from the SoC variant JSON (no explicit
        # `memory_map:` in the preset) carries name/size but NO `base`
        # until the SoM is HW-mapped.  Treat a missing base the same as
        # an explicit `TBD` so an un-mapped SoM (e.g. AEN801, whose E8
        # SoC JSON has no per-region base yet) lands a clean *blocked*
        # carve-out instead of crashing with KeyError: 'base'.
        base = region.get("base")
        size_bytes = _region_size_bytes(region)
        base_is_unmapped = base is None or is_tbd(base)
        if base_is_unmapped:
            return None, (
                f"memory_map.base is {'unset' if base is None else 'TBD'} "
                f"for region '{name}' in SoM {project.sku}; this SoM "
                f"hasn't been HW-mapped yet so IPC carve-outs cannot be "
                f"allocated.  Add a `memory_map:` block to "
                f"metadata/e1m_modules/{project.sku}.yaml (or per-region "
                f"`base`) or remove the matching ipc entry from board.yaml.")
        if size_bytes is None:
            return None, (
                f"memory_map.size is unresolvable for region '{name}' "
                f"in SoM {project.sku} (size_mib / size_kib unset or "
                f"TBD).  Cannot allocate carve-outs.")
        # Top-down allocator: top = base + size, page-aligned.
        top = base + size_bytes
        top = _align_down(top, _PAGE)
        region_top[name] = top
        return top, None

    def _endpoint_window(
        region: dict[str, Any],
        endpoint_set: set[str],
    ) -> tuple[int, int]:
        """Sub-extent of `region` that EVERY endpoint can actually address.

        `accessible_from:` is a yes/no per core; it cannot say "this core
        reaches only part of the region".  The RZ/V2N Cortex-M33 does
        exactly that: per Renesas FSP `bsp_slave_address.h` its DDR window
        is CM33-secure `0x80000000` / CM33-non-secure `0x90000000` -> A55
        `0x40000000`, **256 MiB**, while `ddr_main` spans 4 GiB from
        `0x48000000`.  A top-down allocation therefore handed the M33
        `0x147f80000` -- a 33-bit address that truncates to `0x47f80000`,
        below the DDR base, when cast to a pointer on the M33 (#553).

        `access_windows:` (optional, per region, keyed by core id) declares
        the reachable slice as an absolute `base` + `size_mib`/`size_kib` in
        the same address space as the region's own `base`.  A region that
        declares none is reachable in full, so this returns the whole
        extent and nothing changes for it.
        """
        lo = int(region["base"])
        hi = lo + (_region_size_bytes(region) or 0)
        windows = region.get("access_windows") or {}
        if not isinstance(windows, dict):
            return lo, hi
        for core in sorted(endpoint_set):
            win = windows.get(core)
            if not isinstance(win, dict):
                continue
            win_lo = win.get("base")
            win_size = _region_size_bytes(win)
            if not isinstance(win_lo, int) or win_size is None:
                continue
            lo = max(lo, win_lo)
            hi = min(hi, win_lo + win_size)
        return lo, hi

    def _first_free_down(
        top: int,
        size_aligned: int,
        spans: list[tuple[int, int, str]],
        floor: int,
    ) -> int:
        """First page-aligned base at or below `top` colliding with nothing.

        The top-down allocator must allocate AROUND spans already pinned by
        an explicit `ipc[].address:`, not merely notice the collision --
        refusing instead of skipping would break the ordinary case (a
        project that pins one channel and lets the rest float).  Returns a
        base below `floor` when the region has no room left; the caller
        reports that as a blocked entry.
        """
        base = _align_down(top - size_aligned, _PAGE)
        moved = True
        while moved and base >= floor:
            moved = False
            for lo, hi, _ in sorted(spans, reverse=True):
                if base < hi and lo < base + size_aligned:
                    base = _align_down(lo - size_aligned, _PAGE)
                    moved = True
        return base

    def _candidate_regions(
        entry: IpcEntry,
    ) -> tuple[list[dict[str, Any]], Optional[str]]:
        """Regions this entry may land in, ranked; or a block reason."""
        prefers_cacheable = (bool(entry.cacheable)
                             if entry.cacheable is not None else False)
        endpoint_set = set(entry.endpoints)

        # Filter candidates: accessibility covers every endpoint, and the
        # region isn't marked non-allocatable for IPC carve-outs
        # (`carveout: false` -- e.g. a flash-class MRAM sub-region that
        # must publish a real `base` for the board generator's partition
        # table but isn't byte-addressable shared memory a raw_shmem/
        # rpmsg carve-out can safely land in).
        candidates: list[dict[str, Any]] = []
        excluded_names: list[str] = []
        for region in memory_map:
            af = set(region.get("accessible_from") or [])
            if not endpoint_set.issubset(af):
                continue
            if region.get("carveout") is False:
                excluded_names.append(region["name"])
                continue
            candidates.append(region)
        if not candidates:
            if excluded_names:
                return [], (
                    f"ipc entry '{entry.name}' endpoints {entry.endpoints} "
                    f"only match memory_map region(s) {excluded_names} in "
                    f"SoM {project.sku}, and all of them are marked "
                    f"`carveout: false` (flash-class, not safe as shared "
                    f"memory); add an allocatable region (`carveout: true` "
                    f"or omitted) to metadata/e1m_modules/{project.sku}.yaml "
                    f"or remove the matching ipc entry from board.yaml.")
            return [], (
                f"ipc entry '{entry.name}' endpoints {entry.endpoints} "
                f"have no matching memory_map region in SoM "
                f"{project.sku}")

        # Prefer the region whose `cacheable:` flag matches the entry's
        # preference.  Default carve-out is non-cacheable.
        def _rank(r: dict[str, Any]) -> tuple[int, int]:
            cacheable_match = (bool(r.get("cacheable")) == prefers_cacheable)
            # Smaller region size first (avoid eating the giant DDR
            # region with tiny carve-outs when ocram fits).
            size_b = _region_size_bytes(r) or 1 << 62
            return (0 if cacheable_match else 1, size_b)

        candidates.sort(key=_rank)
        return candidates, None

    def _carve_size_aligned(entry: IpcEntry) -> int:
        carve_size = entry.carve_out_kb * 1024
        return ((carve_size + _PAGE - 1) // _PAGE) * _PAGE

    def _overlap(
        region_name: str,
        base: int,
        size_aligned: int,
    ) -> Optional[tuple[int, int, str]]:
        top = base + size_aligned
        for lo, hi, peer in placed_spans.get(region_name, []):
            if not (top <= lo or base >= hi):
                return (lo, hi, peer)
        return None

    def _place_pinned(
        entry: IpcEntry,
    ) -> tuple[Optional[dict[str, Any]], int, Optional[str]]:
        """Honour an explicit `ipc[].address:` -- bounds- and overlap-checked."""
        candidates, block_reason = _candidate_regions(entry)
        if block_reason is not None:
            return None, 0, block_reason

        base = entry.address or 0
        if base % _PAGE != 0:
            return None, 0, (
                f"ipc entry '{entry.name}' explicit address "
                f"0x{base:x} is not page-aligned (4 KiB)")

        size_aligned = _carve_size_aligned(entry)
        endpoint_set = set(entry.endpoints)

        # The pinned range decides the region -- the ranked-first region is
        # only a PREFERENCE, and labelling a pin with it was how
        # `0xdeadb000` came back tagged `region: ocram_low` (#552).
        windows: list[str] = []
        chosen: Optional[dict[str, Any]] = None
        tbd_reason: Optional[str] = None
        for region in candidates:
            _top, region_block_reason = _region_top_init(region)
            if region_block_reason is not None:
                tbd_reason = tbd_reason or region_block_reason
                continue
            lo, hi = _endpoint_window(region, endpoint_set)
            windows.append(f"{region['name']} 0x{lo:x}..0x{hi:x}")
            if lo <= base and base + size_aligned <= hi:
                chosen = region
                break
        if chosen is None:
            if not windows:
                return None, 0, (tbd_reason or (
                    f"ipc entry '{entry.name}' explicit address 0x{base:x} "
                    f"cannot be checked: no candidate region in SoM "
                    f"{project.sku} carries a resolvable base + size"))
            return None, 0, (
                f"ipc entry '{entry.name}' explicit address range "
                f"0x{base:x}..0x{base + size_aligned:x} lies outside every "
                f"memory_map region endpoints {entry.endpoints} can reach "
                f"on SoM {project.sku} (reachable: {', '.join(windows)}); "
                f"drop the `address:` override to let the allocator place "
                f"it, or move it inside one of those windows.")

        region_name = chosen["name"]
        clash = _overlap(region_name, base, size_aligned)
        if clash is not None:
            return None, 0, (
                f"ipc entry '{entry.name}' explicit address 0x{base:x} "
                f"({entry.carve_out_kb} KiB) overlaps carve-out "
                f"'{clash[2]}' already placed at 0x{clash[0]:x}..0x"
                f"{clash[1]:x} in region '{region_name}'")

        placed_spans.setdefault(region_name, []).append(
            (base, base + size_aligned, entry.name))
        # A pin does NOT move the bump allocator's high-water mark -- it may
        # sit deliberately low -- but `placed_spans` now makes the allocator
        # step over it.
        return chosen, base, None

    def _place_auto(
        entry: IpcEntry,
    ) -> tuple[Optional[dict[str, Any]], int, Optional[str]]:
        """Allocate top-down inside the endpoint-reachable window."""
        candidates, block_reason = _candidate_regions(entry)
        if block_reason is not None:
            return None, 0, block_reason

        chosen = candidates[0]
        region_name = chosen["name"]

        # Initialise the region (and surface any TBD field).  Blocked
        # regions emit a blocked entry rather than aborting.
        _top, region_block_reason = _region_top_init(chosen)
        if region_block_reason is not None:
            return None, 0, region_block_reason

        size_aligned = _carve_size_aligned(entry)
        window_lo, window_hi = _endpoint_window(chosen, set(entry.endpoints))
        # Descend from whichever is lower: the region's remaining top, or
        # the ceiling of the reachable window (#553).
        start = _align_down(min(region_top[region_name], window_hi), _PAGE)
        base = _first_free_down(
            start, size_aligned, placed_spans.get(region_name, []), window_lo)
        if base < window_lo:
            region_lo = int(chosen["base"])
            region_hi = region_lo + (_region_size_bytes(chosen) or 0)
            window_note = ""
            if (window_lo, window_hi) != (region_lo, region_hi):
                window_note = (
                    f"; endpoints {entry.endpoints} reach only "
                    f"0x{window_lo:x}..0x{window_hi:x} of it "
                    f"(0x{region_lo:x}..0x{region_hi:x})")
            return None, 0, (
                f"ipc entry '{entry.name}' ({entry.carve_out_kb} "
                f"KiB) doesn't fit in region '{region_name}' "
                f"after prior allocations{window_note}")

        region_top[region_name] = base
        placed_spans.setdefault(region_name, []).append(
            (base, base + size_aligned, entry.name))
        return chosen, base, None

    # Sort entries alphabetically by name for determinism.
    sorted_entries = sorted(project.ipc, key=lambda e: e.name)

    # Placement runs in TWO passes: explicit `address:` pins first, then
    # the bump allocator around them.  A single name-sorted pass let an
    # auto-allocated `a_chan` take `0x80000` before the pinned `b_chan`
    # asked for it, and both came back `ok` on the same address (#552).
    # Emission below still walks `sorted_entries`, so ordering -- and the
    # endpoint-id collision report -- is unchanged.  Keyed by INDEX, not
    # name: board.schema.json does not require `ipc[].name` to be unique
    # (a duplicate is caught by the endpoint-id check further down), and a
    # name-keyed map would silently give the twins one placement.
    placeable = [(i, e) for i, e in enumerate(sorted_entries)
                 if not (e.kind == "rpmsg" and rpmsg_block_reason is not None)]
    placement: dict[int, tuple[Optional[dict[str, Any]], int, Optional[str]]] = {}
    for index, entry in placeable:
        if entry.address is not None:
            placement[index] = _place_pinned(entry)
    for index, entry in placeable:
        if entry.address is None:
            placement[index] = _place_auto(entry)

    resolved: list[ResolvedCarveOut] = []
    seen_low_bytes: dict[int, str] = {}

    for index, entry in enumerate(sorted_entries):
        # Mailbox metadata blocked? rpmsg entries can't proceed.
        if entry.kind == "rpmsg" and rpmsg_block_reason is not None:
            resolved.append(_blocked_carve_out(entry, rpmsg_block_reason))
            continue

        chosen, base, place_reason = placement[index]
        if place_reason is not None or chosen is None:
            resolved.append(_blocked_carve_out(
                entry, place_reason or "carve-out could not be placed"))
            continue
        region_name = chosen["name"]
        carve_size_aligned = _carve_size_aligned(entry)

        # Endpoint ID derivation.
        h = _fnv1a_32(entry.name.encode("utf-8"))
        low = h & 0x0FF
        if low in seen_low_bytes:
            resolved.append(_blocked_carve_out(entry, (
                f"ipc entry '{entry.name}' endpoint-id low byte "
                f"0x{low:02x} collides with prior entry "
                f"'{seen_low_bytes[low]}'.  Rename one of the channels.")))
            continue
        seen_low_bytes[low] = entry.name
        src_ept = 0x400 | low
        dst_ept = src_ept + 1

        mbox = _resolve_default_mailbox_channel(mailbox, entry.name)

        resolved.append(ResolvedCarveOut(
            name=entry.name,
            kind=entry.kind,
            endpoints=list(entry.endpoints),
            base=base,
            size=carve_size_aligned,
            region=region_name,
            cacheable=bool(chosen.get("cacheable", False))
                       if entry.cacheable is None
                       else bool(entry.cacheable),
            src_ept=src_ept,
            dst_ept=dst_ept,
            mailbox_channel=mbox,
        ))

    return resolved
