#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""System-manifest emitter -- assembles system-manifest.yaml from the model.

`emit_system_manifest` renders the spec-§5.2 manifest (slices, carve-outs,
storage, helper-MCU block) off the parsed BoardProject + the resolved carve-outs
/ partitions; `_helper_mcus` builds the manifest's `helper_mcus[]` block (shared
with the Orchestrator's materialise path, which back-imports it). Extracted as
the #285 manifest emit seam.
"""

from __future__ import annotations

from typing import Any, Optional

import yaml

from .carveout import resolve_carve_outs
from .models import BoardProject, Slice, SystemManifest
from .partition import resolve_storage_partitions


def emit_system_manifest(
    project: BoardProject,
    *,
    slices: Optional[list[Slice]] = None,
) -> str:
    """Generate system-manifest.yaml per spec §5.2.

    If `slices` is None, projects the BoardProject's `cores` dict
    as-is (typical "describe what will run" call).  When the
    orchestrator finishes fan_out it passes its updated Slice list
    so the manifest carries status / log_path / etc.
    """
    carve_outs = resolve_carve_outs(project)
    partitions = resolve_storage_partitions(project)
    effective_slices = list(slices) if slices is not None else list(project.cores.values())

    boot_order = list(project.som_preset.get("boot_order") or [])

    manifest = SystemManifest(
        project=project,
        slices=effective_slices,
        carve_outs=carve_outs,
        partitions=partitions,
        boot_order=boot_order,
        helper_mcus=_helper_mcus(project),
    )

    out = manifest.to_dict()
    # Comment when boot_order is empty so reviewers see the gap.
    text = yaml.safe_dump(out, sort_keys=False, default_flow_style=False)
    if not boot_order:
        text += ("\n# boot_order is empty -- add a `boot_order:` list to "
                 f"metadata/e1m_modules/{project.sku}.yaml when the\n"
                 "# bring-up order is finalised.\n")
    return text


def _helper_mcus(project: BoardProject) -> list[dict[str, Any]]:
    """Build the manifest's `helper_mcus[]` block.

    Two sources contribute:

    1. The SoM preset's `helper_firmware:` list (Phase 3) -- carries
       authoritative firmware_path + flash_method + flash_args; each
       entry projects verbatim into the manifest.  Entries whose
       firmware_path is `TBD` still land in the manifest with a
       human-readable note so reviewers see the gap (the orchestrator
       does NOT fail the build on TBD helper firmware -- the
       Renesas + Alif flash flows are independently scriptable).

    2. Legacy `on_module.{supervisor_mcu,wifi_ble}` strings (kept
       for back-compat with the pre-Phase-3 metadata shape) -- only
       added if the SKU has no explicit helper_firmware list, so
       Phase 1 presets that haven't yet been extended still surface
       their helper MCUs in the manifest.
    """
    out: list[dict[str, Any]] = []

    helper_firmware = project.som_preset.get("helper_firmware")
    if isinstance(helper_firmware, list):
        for entry in helper_firmware:
            if not isinstance(entry, dict):
                continue
            row: dict[str, Any] = {
                "name":          entry.get("name"),
                "chip":          entry.get("chip"),
                "firmware_path": entry.get("firmware_path"),
                "flash_method":  entry.get("flash_method"),
                "flash_args":    entry.get("flash_args"),
            }
            if entry.get("firmware_path") == "TBD":
                row["note"] = ("firmware_path TBD; populated when the "
                               "upstream firmware release lands")
            out.append(row)
        return out

    # Back-compat path -- only invoked when the preset is still on
    # the pre-Phase-3 shape (no helper_firmware: block at all).
    om = project.som_preset.get("on_module") or {}
    for key in ("supervisor_mcu", "wifi_ble"):
        val = om.get(key)
        if val:
            out.append({
                "name":          val,
                "role":          key,
                "firmware_path": None,
                "flash_method":  None,
            })
    return out
