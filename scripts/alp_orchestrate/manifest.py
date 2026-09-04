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

    Source: the SoM preset's `helper_firmware:` list (Phase 3).  Each
    key the entry declares is projected INDEPENDENTLY -- how the image
    is written locally (flash_method + flash_args), how it is updated in
    the field (update_channel), and who may invoke the flash method
    (flash_policy) are three separate axes, and a helper may declare any
    combination.  The GD32 bridge declares all three: an
    `alp_ota_spi_bridge` channel for normal field updates AND a
    `recovery_only` swd_probe method for a bricked board.  Dropping the
    flash keys because a channel exists would DELETE that recovery path
    from the manifest rather than let `tan flash` decline it, so this
    function must never make one key's presence suppress another's
    (#1357).  `firmware_path` is entirely ABSENT from
    the row when the preset doesn't declare one (e.g. GD32 bridge SKUs
    pending a released binary, #852/#936) -- it is never emitted as
    `null`, because `system-manifest-v1.schema.json` types it `string`
    when present (the orchestrator does NOT fail the build on a missing
    helper firmware path -- the Renesas + Alif flash flows are
    independently scriptable).

    Every in-tree SoM preset carries a `helper_firmware:` list (no more
    Phase-1 stragglers), so there is no back-compat `on_module.{
    supervisor_mcu,wifi_ble}` fallback here -- no-legacy-compat.
    """
    out: list[dict[str, Any]] = []

    helper_firmware = project.som_preset.get("helper_firmware")
    if isinstance(helper_firmware, list):
        for entry in helper_firmware:
            if not isinstance(entry, dict):
                continue
            row: dict[str, Any] = {
                "name": entry.get("name"),
                "chip": entry.get("chip"),
            }
            # Project every declared key independently.  No key's
            # presence suppresses another's: `update_channel` describes
            # field updates, `flash_method`/`flash_args` describe a local
            # write, and `flash_policy` says who may perform that write.
            # A helper declaring both halves keeps both -- `tan flash`
            # decides what to do with them from `flash_policy` (#1357).
            for key in ("firmware_path", "flash_method", "flash_args",
                        "flash_policy", "update_channel"):
                value = entry.get(key)
                if value is not None:
                    row[key] = value
            out.append(row)
    return out
