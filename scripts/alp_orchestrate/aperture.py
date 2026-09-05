#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""SoC on-die flash aperture resolution + region classification (#1365 split B).

`resolve_aperture()` was `check_atoc_reservation.py`'s private `_resolve_aperture`
(#1365 split A) -- moved here, unchanged in behaviour, so `carveout.py` and
`partition.py` can derive the same `flash` / `ram` / `unclassified` /
`unresolved` verdict the gate already computes, instead of re-deriving the
aperture math a second time. `check_atoc_reservation.py` now imports this
module instead of carrying its own copy.

Depends only downward -- `alp_project_loader` (`resolve_soc_path`,
`_resolve_silicon_variant`) and `memregion` (`_region_size_bytes`); nothing
calls back into the `alp_orchestrate` package.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Optional

from alp_project_loader import _resolve_silicon_variant, resolve_soc_path

from .memregion import _region_size_bytes

# The four verdicts `classify_region()` can return.  Not an enum -- every
# caller compares against a string literal, matching the rest of this
# codebase's metadata-classification helpers (e.g. `resolve_memory_map`'s
# region dicts).
RegionClass = str  # "flash" | "ram" | "unclassified" | "unresolved"


def resolve_aperture(
    preset: dict[str, Any],
    metadata_root: Path,
) -> Optional[tuple[int, int]]:
    """Resolve a SoM preset's declared on-die MRAM aperture as `[base, top)`.

    `base` comes from the preset's SoC's `soc_flash_base`; `top` is
    `base + variants[].mram_mb * 1 MiB` for the VARIANT the preset resolves
    to -- never the SoC's top-level `soc_flash_mb`, because an E3 ships a
    5.5 MB and a 1.5 MB variant off the same base
    (metadata/socs/alif/ensemble/e3.json). `resolve_soc_path` and
    `_resolve_silicon_variant` (`alp_project_loader`, #997) are the single
    source for the SoC-path and variant resolution; this function does not
    re-derive the vendor/family/part split.

    Returns None -- "no aperture declared, skip every aperture-anchored
    check" -- when the preset names no SoC, the SoC omits `soc_flash_base`,
    or the resolved variant has no usable `mram_mb`. Never guesses
    (ADR-0034 clause 4).
    """
    soc_path = resolve_soc_path(preset.get("silicon"), metadata_root)
    if soc_path is None or not soc_path.is_file():
        return None
    try:
        soc_spec = json.loads(soc_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(soc_spec, dict):
        return None
    base = soc_spec.get("soc_flash_base")
    if not isinstance(base, int) or isinstance(base, bool):
        return None
    variant = _resolve_silicon_variant(preset, metadata_root)
    if not isinstance(variant, dict):
        return None
    mram_mb = variant.get("mram_mb")
    if not isinstance(mram_mb, (int, float)) or isinstance(mram_mb, bool):
        return None
    top = base + int(round(mram_mb * 1024 * 1024))
    return base, top


def region_extent(region: dict[str, Any]) -> Optional[tuple[int, int]]:
    """`[base, base + size)` for one `memory_map:` region, or None when the
    base is unresolved (the `"TBD"` string, or absent) or the size can't be
    read. Callers must skip an unresolved extent rather than guessing at it
    (ADR-0034 clause 4) -- never fold it into a gap or a class verdict.
    """
    base = region.get("base")
    if not isinstance(base, int) or isinstance(base, bool):
        return None
    size_bytes = _region_size_bytes(region)
    if size_bytes is None:
        return None
    return base, base + size_bytes


def classify_region(
    region: dict[str, Any],
    aperture: Optional[tuple[int, int]],
    is_preset_authored: bool,
) -> RegionClass:
    """Classify one `memory_map:` row against the SoC's declared aperture
    (#1365 split B).

    Four verdicts, in priority order:

      - `"unresolved"` -- the region's own `base` doesn't resolve, OR no
        aperture is declared for this SoC at all (`aperture is None`).
        There is nothing to compare in either case; callers must honour an
        authored flag (`write_authority`, or the legacy `carveout`) if one
        is present rather than guess (ADR-0034 clause 4). This is also
        what makes split B a provable no-op on every non-Alif SoM: with no
        declared aperture, EVERY row on that SoM lands here, so a caller
        that special-cases `aperture is None` before ever calling this
        function reproduces the pre-split-B behaviour byte-for-byte.

      - `"ram"` -- the region was NOT authored by the SoM preset itself
        (a SoC-level `memory_regions` row from soc-spec-v1, or a row
        `alp_project_loader.resolve_memory_map`'s derivation branch built
        from the silicon variant). RAM by construction, needs no
        authority -- these rows are what keep every V2N/V2M/NX9101
        derivation byte-identical.

      - `"flash"` -- the region's resolved extent is CONTAINED in the
        aperture (including the whole-device alias case, extent ==
        aperture exactly, e.g. `mram_main` once its `base` stops being
        `"TBD"`). Containment is proof: the on-die MRAM aperture holds
        nothing else.

      - `"unclassified"` -- the region resolves a base OUTSIDE the
        aperture. Containment is ONE-DIRECTIONAL (#1365 split A/B):
        outside proves NOTHING -- Ensemble's OSPI XIP windows sit outside
        `[soc_flash_base, ...)` and are still flash, and the same OSPI0
        controller carries a HyperRAM alongside the NOR on a different
        chip_select. A preset-authored row landing here needs
        `write_authority: customer_runtime` to be IPC-eligible (P1); a
        future authored OSPI XIP row must NOT silently become an IPC
        candidate just because it resolves outside the aperture.
    """
    ext = region_extent(region)
    if ext is None:
        return "unresolved"
    if aperture is None:
        return "unresolved"
    if not is_preset_authored:
        return "ram"
    lo, hi = ext
    full_lo, full_hi = aperture
    if lo == full_lo and hi == full_hi:
        return "flash"  # whole-device alias -- the device itself
    if lo >= full_lo and hi <= full_hi:
        return "flash"  # strictly contained -- a partition inside the device
    return "unclassified"  # outside the aperture -- proves nothing


def is_partition_inside_aperture(
    region: dict[str, Any],
    aperture: Optional[tuple[int, int]],
) -> Optional[bool]:
    """P2: is `region` a partition INSIDE a flash device, not a device of
    its own?

    True when the region's extent is a PROPER subset of `aperture` (e.g.
    AEN's `mcuboot` / `he_slot0` / `hp_slot0` / `reserved` / `storage` /
    `atoc`, each a fine-grained slice of the on-die MRAM window); False
    when the extent equals the aperture exactly (the region IS the
    device -- `mram_main`, once resolved) or lies outside it entirely
    (a legitimate device the aperture doesn't cover, e.g. an OSPI part).
    None when the extent or the aperture itself can't be resolved --
    callers must fall back to the legacy `carveout:` flag rather than
    guess (ADR-0034 clause 4), which is also what keeps this a no-op
    wherever the aperture never resolves (every non-Alif SoM) or the
    row's own base is still TBD (`mram_main` today).
    """
    if aperture is None:
        return None
    ext = region_extent(region)
    if ext is None:
        return None
    lo, hi = ext
    full_lo, full_hi = aperture
    if lo == full_lo and hi == full_hi:
        return False  # the device itself
    if lo >= full_lo and hi <= full_hi:
        return True  # proper subset -- a partition inside the device
    return False  # outside the aperture -- not this device's partition
