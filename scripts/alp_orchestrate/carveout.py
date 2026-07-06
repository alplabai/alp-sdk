#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Carve-out resolver -- board.yaml `ipc:` entries -> resolved shared-memory regions.

Each IPC entry maps to a `ResolvedCarveOut`: the shared-memory base/size come
from the SoM's resolved memory_map, the region id is a deterministic FNV-1a
hash, plus the default mailbox channel and page alignment. An entry that can't
resolve (missing mailbox metadata / no memory_map) becomes a blocked
ResolvedCarveOut carrying the reason. Extracted from alp_orchestrate as the #285
carve-out seam.

Depends only downward -- models (dataclasses), paths (METADATA_ROOT), and
alp_project.resolve_memory_map; nothing calls back into the package __init__.
"""

from __future__ import annotations

from typing import Any, Optional

from alp_project import resolve_memory_map

from .models import BoardProject, IpcEntry, ResolvedCarveOut
from .memregion import _PAGE, _region_size_bytes
from .paths import METADATA_ROOT



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
    4. Allocate top-down within the region, page-aligned (4 KiB).
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
    memory_map = resolve_memory_map(project.som_preset, METADATA_ROOT)
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
        if controller is None or controller == "TBD":
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
        base_is_unmapped = (
            base is None
            or (isinstance(base, str) and base.strip().upper() == "TBD")
        )
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

    # Sort entries alphabetically by name for determinism.
    sorted_entries = sorted(project.ipc, key=lambda e: e.name)

    resolved: list[ResolvedCarveOut] = []
    seen_low_bytes: dict[int, str] = {}

    for entry in sorted_entries:
        # Mailbox metadata blocked? rpmsg entries can't proceed.
        if entry.kind == "rpmsg" and rpmsg_block_reason is not None:
            resolved.append(_blocked_carve_out(entry, rpmsg_block_reason))
            continue

        prefers_cacheable = bool(entry.cacheable) if entry.cacheable is not None else False
        endpoint_set = set(entry.endpoints)

        # Filter candidates: accessibility covers every endpoint.
        candidates: list[dict[str, Any]] = []
        for region in memory_map:
            af = set(region.get("accessible_from") or [])
            if not endpoint_set.issubset(af):
                continue
            candidates.append(region)
        if not candidates:
            resolved.append(_blocked_carve_out(entry, (
                f"ipc entry '{entry.name}' endpoints {entry.endpoints} "
                f"have no matching memory_map region in SoM "
                f"{project.sku}")))
            continue

        # Prefer the region whose `cacheable:` flag matches the entry's
        # preference.  Default carve-out is non-cacheable.
        def _rank(r: dict[str, Any]) -> tuple[int, int]:
            cacheable_match = (bool(r.get("cacheable")) == prefers_cacheable)
            # Smaller region size first (avoid eating the giant DDR
            # region with tiny carve-outs when ocram fits).
            size_b = _region_size_bytes(r) or 1 << 62
            return (0 if cacheable_match else 1, size_b)

        candidates.sort(key=_rank)
        chosen = candidates[0]
        region_name = chosen["name"]

        # Initialise the region (and surface any TBD field).  Blocked
        # regions emit a blocked entry rather than aborting.
        _top, region_block_reason = _region_top_init(chosen)
        if region_block_reason is not None:
            resolved.append(_blocked_carve_out(entry, region_block_reason))
            continue
        carve_size = entry.carve_out_kb * 1024
        carve_size_aligned = ((carve_size + _PAGE - 1) // _PAGE) * _PAGE

        # Honour an explicit address override per ipc_entry.
        if entry.address is not None:
            base = entry.address
            if base % _PAGE != 0:
                resolved.append(_blocked_carve_out(entry, (
                    f"ipc entry '{entry.name}' explicit address "
                    f"0x{base:x} is not page-aligned (4 KiB)")))
                continue
        else:
            new_top = region_top[region_name] - carve_size_aligned
            region_lo = int(chosen["base"])
            if new_top < region_lo:
                resolved.append(_blocked_carve_out(entry, (
                    f"ipc entry '{entry.name}' ({entry.carve_out_kb} "
                    f"KiB) doesn't fit in region '{region_name}' "
                    f"after prior allocations")))
                continue
            region_top[region_name] = new_top
            base = new_top

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
