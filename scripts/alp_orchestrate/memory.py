#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Resolved memory-region view for the system manifest (#1365 item 3).

`resolve_memory_regions()` projects the SoM's effective memory-region table
into the `memory[]` pane `system-manifest-v1` declares -- the third pane
`ipc[]` and `storage[]` refer INTO by name, not a third copy of them.

Three vocabularies, each taken from what the code already produces rather
than from #1365's issue body (which predates splits A and B and proposes
shapes the shipped derivation cannot fill):

* `kind` is `aperture.classify_region()`'s own four verdicts --
  `flash` / `ram` / `unclassified` / `unresolved`. Collapsing them onto the
  issue's two (`ram` | `flash`) would have to invent a class for the two
  verdicts that exist precisely to say "not proven": `unclassified` (outside
  the aperture AND preset-authored -- an Ensemble OSPI XIP window is outside
  and still flash) and `unresolved` (no extent, or no aperture declared for
  this SoC at all, which is every non-Alif SoM).

* the authority field is `write_authority`, passed through verbatim with
  som-preset-v1's six values. The issue's 3-value `owner` was replaced
  during review because one axis cannot separate flash-time from runtime
  authority: `customer_image` and `customer_runtime` would both collapse to
  `customer`, which is the exact distinction the AEN `storage`-vs-`he_slot0`
  hazard needs. A row that authors no `write_authority` carries no key --
  absent means unresolved, never `customer_runtime` (ADR-0034 clause 4).

* `status` is `ok` / `unresolved`, the word this schema's own items
  description already made normative, not `ipc[]`'s `ok` / `blocked`. They
  are not synonyms: `blocked` is a verdict about an allocation request,
  while a region is a fact that either resolved an address or did not.

`source` is derived here rather than tagged in `alp_project.resolve_memory_map`
on purpose. That function's precedence is ALL-OR-NOTHING (an authored
`memory_map:` wins the whole table, else the SoC's fixed `memory_regions`,
else the silicon-variant derivation), so provenance is a property of the
table, not of a row -- `carveout.py` and `partition.py` already read it the
same way, via `bool(som_preset.get("memory_map"))`. Tagging rows inside the
loader would also add a key to dicts that flow into `$defs/memory_region`'s
`additionalProperties: false` validation path.

Depends only downward -- `alp_project.resolve_memory_map`, `aperture` and
`memregion`; nothing calls back into the package.
"""

from __future__ import annotations

from typing import Any, Optional

from alp_project import resolve_memory_map

from .aperture import classify_region, resolve_aperture
from .memregion import _region_size_bytes
from .models import BoardProject

# The two provenance values a region row can carry.  `board_yaml` (the third
# value #1365's issue body proposes) has no producer: board.yaml never
# AUTHORS a region, it only NAMES one via `ipc[].region` / `storage[].
# flash_device`, so a board_yaml-sourced region row cannot occur.
_SOURCE_PRESET = "som_preset"
_SOURCE_SOC = "soc_derived"


def resolve_memory_regions(project: BoardProject) -> list[dict[str, Any]]:
    """Project the SoM's memory regions into the manifest's `memory[]` pane.

    Returns `[]` when the SoM resolves no regions at all (an unresolvable
    `silicon_variant`, e.g. NX9101's `TBD`). The caller must then OMIT the
    key rather than emit `memory: []` -- the schema draws a load-bearing
    distinction between an absent pane ("this producer does not emit it
    yet") and an empty one, and collapsing the two would tell every
    consumer that a SoM with a pending HW-config writeup has no memory.
    """
    metadata_root = project.effective_metadata_root()
    memory_map = resolve_memory_map(project.som_preset, metadata_root)
    if not memory_map:
        return []

    # Same two signals `carveout.py` / `partition.py` derive, read the same
    # way: the SoC's declared on-die flash aperture (None on every non-Alif
    # SoM), and whether THIS preset authored the table.
    aperture = resolve_aperture(project.som_preset, metadata_root)
    is_preset_authored = bool(project.som_preset.get("memory_map"))
    source = _SOURCE_PRESET if is_preset_authored else _SOURCE_SOC

    return [
        _resolved_row(region, aperture, is_preset_authored, source)
        for region in memory_map
        if isinstance(region, dict) and region.get("name")
    ]


def _resolved_row(
    region: dict[str, Any],
    aperture: Optional[tuple[int, int]],
    is_preset_authored: bool,
    source: str,
) -> dict[str, Any]:
    """Shape one region into a manifest row.

    Key order is deliberate and load-bearing for the byte-golden `.snap`
    fixtures: identity first (`name`, `source`), then the two derived
    verdicts (`kind`, `status`), then the resolved numbers, then the
    authored passthroughs, then the explanation.  Builds a NEW dict --
    the loader's region dicts are never mutated.
    """
    row: dict[str, Any] = {
        "name":   str(region.get("name")),
        "source": source,
        "kind":   classify_region(region, aperture, is_preset_authored),
    }

    # `status` keys off the BASE, not off the extent: a region whose base
    # resolves but whose size does not has still resolved its address, and
    # the schema's rule is written about `base` ("carries no `base` and says
    # why").  `region_extent()` folds both into one None, so it cannot
    # answer this question on its own.
    base = region.get("base")
    base_ok = isinstance(base, int) and not isinstance(base, bool)
    row["status"] = "ok" if base_ok else "unresolved"
    if base_ok:
        row["base"] = base

    size_bytes = _region_size_bytes(region)
    if size_bytes is not None:
        row["size_bytes"] = size_bytes

    write_authority = region.get("write_authority")
    if isinstance(write_authority, str) and write_authority:
        row["write_authority"] = write_authority

    accessible_from = region.get("accessible_from")
    if isinstance(accessible_from, list) and accessible_from:
        row["accessible_from"] = [str(core) for core in accessible_from]

    if not base_ok:
        row["reason"] = _unresolved_reason(row["name"], base, size_bytes)

    return row


def _unresolved_reason(name: str, base: Any, size_bytes: Optional[int]) -> str:
    """Say WHY a base did not resolve, in terms the author can act on.

    Never a guessed address (ADR-0034 clause 4): the reason names the field
    that is missing and where to declare it, which is what makes an
    unresolved row actionable instead of merely absent.
    """
    if base is None:
        cause = ("carries no `base:` -- the silicon default applies and no "
                 "address is declared for it")
    elif isinstance(base, str):
        cause = f"declares `base: {base}`, a placeholder, not an address"
    else:
        cause = f"declares a non-integer `base:` ({type(base).__name__})"
    sizing = (" Its size resolves, so only the address is missing."
              if size_bytes is not None else
              " Neither its address nor its size resolves.")
    return (f"Region '{name}' {cause}, so no extent can be resolved and no "
            f"class derived.{sizing} Declare `base:` in this SoM preset's "
            f"`memory_map:` to resolve it.")
