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

import hashlib
from typing import Any, Optional

import yaml

from .carveout import resolve_carve_outs
from .models import BoardProject, Slice, SystemManifest
from .partition import resolve_storage_partitions
from .paths import REPO


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


def _artifact_status(firmware_path: Optional[str], sha256: Optional[str]) -> str:
    """Classify a helper-firmware artefact without ever hard-failing.

    `absent` (gitignored build output not yet produced -- the GD32 case on a
    fresh clone) is reported distinctly from `mismatch` (the file is there
    but its hash disagrees with metadata) -- see issue #852.  `verified` is
    the happy path; `unchecked` covers a `TBD` sha256 (nothing to compare
    against yet) and `not-applicable` covers a `TBD`/missing firmware_path.
    """
    if not firmware_path or firmware_path == "TBD":
        return "not-applicable"
    path = REPO / firmware_path
    if not path.is_file():
        return "absent"
    if not sha256 or sha256 == "TBD":
        return "unchecked"
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    return "verified" if actual == sha256 else "mismatch"


def _helper_mcus(project: BoardProject) -> list[dict[str, Any]]:
    """Build the manifest's `helper_mcus[]` block.

    Two sources contribute:

    1. The SoM preset's `helper_firmware:` list (Phase 3) -- carries
       authoritative firmware_path + version + sha256 + flash_method +
       flash_args; each entry projects verbatim into the manifest, plus a
       derived `artifact_status` (issue #852): `verified` (present, hash
       matches), `mismatch` (present, hash disagrees), `absent` (gitignored
       build output not yet produced -- the GD32 case on a fresh clone),
       `unchecked` (sha256 is TBD), or `not-applicable` (firmware_path is
       TBD).  `absent` is a reported condition, NOT a build failure --
       `check_helper_firmware.py` is the CI gate that turns `mismatch` into
       a hard error while leaving `absent` non-fatal.  Entries whose
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
            firmware_path = entry.get("firmware_path")
            sha256 = entry.get("sha256")
            row: dict[str, Any] = {
                "name":            entry.get("name"),
                "chip":            entry.get("chip"),
                "firmware_path":   firmware_path,
                "version":         entry.get("version"),
                "sha256":          sha256,
                "artifact_status": _artifact_status(firmware_path, sha256),
                "flash_method":    entry.get("flash_method"),
                "flash_args":      entry.get("flash_args"),
            }
            if firmware_path == "TBD":
                row["note"] = ("firmware_path TBD; populated when the "
                               "upstream firmware release lands")
            elif row["artifact_status"] == "absent":
                row["note"] = (f"{firmware_path} not found on disk (gitignored "
                                "build output or not yet fetched) -- non-fatal; "
                                "see check_helper_firmware.py")
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
