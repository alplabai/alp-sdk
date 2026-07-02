#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
ALP heterogeneous-OS build orchestrator (Phase 2 of the 2026-05-15 design).

A board.yaml declares per-core runtimes; this module loads the
project, resolves topology defaults from the SoM preset, allocates
IPC carve-outs from the SoM's memory_map, and fans out into one
build slice per non-`off` core.

Public API:

    load_board_yaml(path)          -> BoardProject
    resolve_carve_outs(project)    -> list[ResolvedCarveOut]
    emit_system_manifest(project, slices=...) -> str
    emit_dts_reservations(project) -> str
    emit_ipc_contract_h(project)   -> str
    emit_build_plan(project, board_yaml=..., build_root=...) -> str

    BoardProject, Slice, ResolvedCarveOut, SystemManifest, Orchestrator,
    OrchestratorError

Reference: docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Optional

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("alp_orchestrate: PyYAML is required.  Install via `pip install pyyaml`.")

try:
    import jsonschema  # type: ignore[import-untyped]
except ImportError:
    sys.exit("alp_orchestrate: jsonschema is required.  Install via `pip install jsonschema`.")

# resolve_memory_map derives the effective region table from the SoC variant
# when the SoM preset does not declare an explicit `memory_map:` block.
# resolve_capabilities merges SoC-JSON defaults with SoM-level overrides so
# that silicon-determined caps removed from SoM YAMLs (slice 3b) still resolve.
# Imported here so the orchestrator never duplicates that logic.
from alp_project import (  # noqa: E402
    resolve_memory_map,
)


# The filesystem roots now live in paths.py (the #285 paths seam) -- a leaf both
# __init__ and topology.py import, so topology no longer lazy-imports BOARD_SCHEMA
# back through the package. Re-exported here so `from alp_orchestrate import REPO`
# (and friends) keeps working unchanged.
from .paths import (  # noqa: E402
    BOARD_PRESET_SCHEMA,  # noqa: F401  (re-export for the public surface)
    BOARD_SCHEMA,
    METADATA_ROOT,
    REPO,
)


# The per-core OS-class taxonomy + topology view now live in topology.py (the
# #285 topology seam). Re-export the names referenced outside the module:
# `_default_os_from_core_type` (loader) and the public emit surface `core_os_topology`
# / `emit_os_topology` (alp_project.py + the test_emit_os_topology tests). The
# remaining helpers (CLASS_RUNTIMES, _allowed_os_for_core, _runtime_class) stay
# topology-private -- nothing outside topology.py references them.
from .topology import (  # noqa: E402
    _default_os_from_core_type,
    core_os_topology,  # noqa: F401  (re-export: test_emit_os_topology)
    emit_os_topology,  # noqa: F401  (re-export: alp_project.py + tests)
)

# The orchestrator data model now lives in alp_orchestrate_models (the first
# #285 modularization seam); re-exported here so `from alp_orchestrate import
# Slice` (and friends) keeps working unchanged for callers + tests.
from .models import (  # noqa: E402
    BoardProject,
    IpcEntry,
    OrchestratorError,
    ResolvedCarveOut,  # noqa: F401  (re-export; consumed by carveout.py now, not __init__)
    ResolvedPartition,  # noqa: F401  (re-export; consumed by partition.py + headers.py now)
    Slice,
    StorageEntry,
    SystemManifest,
)


# ---------------------------------------------------------------------
# Silicon ref -> SoC JSON path
# ---------------------------------------------------------------------

def _silicon_to_soc_path(silicon: str, metadata_root: Path) -> Path:
    """`alif:ensemble:e7` -> metadata/socs/alif/ensemble/e7.json."""
    parts = silicon.split(":")
    if len(parts) != 3:
        raise OrchestratorError(
            f"silicon ref '{silicon}' is not a triple-colon string")
    return (metadata_root / "socs" / parts[0] / parts[1] /
            f"{parts[2]}.json")


# ---------------------------------------------------------------------
# Loader
# ---------------------------------------------------------------------


def _load_yaml(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise OrchestratorError(f"file not found: {path}")
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as e:
        raise OrchestratorError(f"failed to parse {path}: {e}") from e
    if not isinstance(data, dict):
        raise OrchestratorError(
            f"{path} did not parse to a top-level mapping")
    return data


def _load_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise OrchestratorError(f"file not found: {path}")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        raise OrchestratorError(f"failed to parse {path}: {e}") from e


def _validate_board(project: dict[str, Any],
                    metadata_root: Path = METADATA_ROOT) -> None:
    schema_path = metadata_root / "schemas" / "board.schema.json"
    if not schema_path.is_file():
        # Fall back to the global schema when the metadata_root doesn't
        # carry its own copy (e.g. synthetic test roots without schemas/).
        schema_path = BOARD_SCHEMA
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    validator = jsonschema.Draft202012Validator(schema)
    errors = sorted(validator.iter_errors(project),
                    key=lambda e: list(e.path))
    if errors:
        messages = []
        for e in errors:
            loc = "/".join(str(p) for p in e.path) or "<root>"
            messages.append(f"  - {loc}: {e.message}")
        raise OrchestratorError(
            "board.yaml schema validation failed:\n" +
            "\n".join(messages))


def _resolve_board_preset(
    preset: str,
    metadata_root: Path,
) -> dict[str, Any]:
    """Load the shared board YAML referenced by `preset:`.

    Shared boards live at metadata/boards/<preset>.yaml.  Raises
    OrchestratorError when the file is missing (`preset:` must resolve;
    customers with a custom board define it inline instead).
    """
    p = metadata_root / "boards" / f"{preset}.yaml"
    if not p.is_file():
        raise OrchestratorError(
            f"`preset: {preset}` does not resolve: no shared board "
            f"at {p.relative_to(REPO) if p.is_relative_to(REPO) else p}. "
            f"Available presets: "
            f"{sorted(_available_presets(metadata_root))}")
    return _load_yaml(p)


def _available_presets(metadata_root: Path) -> list[str]:
    boards_dir = metadata_root / "boards"
    if not boards_dir.is_dir():
        return []
    return [p.stem for p in boards_dir.glob("*.yaml")]


def _synthesize_inline_board(project: dict[str, Any]) -> dict[str, Any]:
    """Build a board-shaped dict from a project's inline top-level fields.

    Used when board.yaml has no `preset:` -- the project's own `name`,
    `populated`, `e1m_routes` (and optional `hw_rev`) double as the
    board definition.  Returned dict has the same shape downstream
    emitters expect from a preset-resolved board.
    """
    return {
        "name":       project.get("name"),
        "populated":  dict(project.get("populated") or {}),
        "e1m_routes": dict(project.get("e1m_routes") or {}),
        # Inline mode has no per-board hw_revisions table; loader code
        # that reads default_hw_rev / hw_revisions tolerates None.
        "default_hw_rev": project.get("hw_rev"),
    }


def _resolve_topology_for_core(
    core_id: str,
    project_cores: dict[str, Any],
    som_topology: dict[str, Any],
) -> Optional[dict[str, Any]]:
    """Per spec §4.5: project's `cores.<id>` overrides the SoM preset's
    `topology.<id>`.  Returns None when neither source has the key."""
    if core_id in project_cores:
        # Customer-supplied entry; merge missing keys from topology.
        merged: dict[str, Any] = dict(som_topology.get(core_id, {}) or {})
        merged.update(project_cores[core_id] or {})
        return merged
    if core_id in som_topology:
        return dict(som_topology[core_id] or {})
    return None


def _slice_from_resolved(
    core_id: str,
    entry: dict[str, Any],
    soc_core_type: str = "",
) -> Slice:
    """Build a Slice dataclass from the resolved per-core entry.

    When `entry["os"]` is missing/empty, the OS is inferred from
    `soc_core_type` via `_default_os_from_core_type()` (cortex-m* ->
    zephyr, cortex-a* -> yocto, else "off").  Passing the empty
    string for `soc_core_type` preserves the historical default of
    "off".
    """
    return Slice(
        core_id=core_id,
        os=str(entry.get("os") or _default_os_from_core_type(soc_core_type)),
        app=entry.get("app"),
        image=entry.get("image"),
        machine=entry.get("machine"),
        board=entry.get("board"),
        toolchain=entry.get("toolchain"),
        peripherals=list(entry.get("peripherals") or []),
        libraries=list(entry.get("libraries") or []),
        extra_libraries=[dict(e) for e in (entry.get("extra_libraries") or [])],
        inference=dict(entry.get("inference") or {}),
        iot=dict(entry.get("iot") or {}),
        memory=dict(entry.get("memory") or {}),
        power=dict(entry.get("power") or {}),
    )


# ---------------------------------------------------------------------
# Cross-field validator helpers (P2.1 + P2.3 of v0.6).
# ---------------------------------------------------------------------


# The cross-field validators now live in validate.py (the #285 validate seam).
# Back-imported for the loader below (its own seam comes next); nothing outside
# the package consumes them.
from .validate import (  # noqa: E402
    _enforce_loader_rules,
    _enforce_os_matches_core_class,
    _validate_consistency,
)


def load_board_yaml(path: Path, *,
                    metadata_root: Path = METADATA_ROOT) -> BoardProject:
    """Load + validate a board.yaml.

    Raises OrchestratorError on any schema / preset / topology error.
    """
    path = Path(path)
    project = _load_yaml(path)

    # 1. Schema validation.  Pass metadata_root so test stubs using
    # non-production SKU patterns (E1M-TST001 etc.) validate against
    # their own copy of the schema rather than the repo's strict pattern.
    _validate_board(project, metadata_root=metadata_root)

    sku = project["som"]["sku"]
    hw_rev = project["som"].get("hw_rev")

    # 2. Resolve SKU preset.
    sku_preset_path = metadata_root / "e1m_modules" / f"{sku}.yaml"
    if not sku_preset_path.is_file():
        raise OrchestratorError(
            f"no preset for SoM SKU {sku} at "
            f"{sku_preset_path.relative_to(REPO) if sku_preset_path.is_relative_to(REPO) else sku_preset_path}")
    som_preset = _load_yaml(sku_preset_path)

    # 3. Resolve SoC spec via the preset's `silicon:` ref.
    silicon = som_preset.get("silicon")
    if not silicon:
        raise OrchestratorError(
            f"SoM preset {sku} has no `silicon:` field")
    soc_path = _silicon_to_soc_path(silicon, metadata_root)
    if not soc_path.is_file():
        raise OrchestratorError(
            f"no SoC spec at {soc_path.relative_to(REPO) if soc_path.is_relative_to(REPO) else soc_path} for ref '{silicon}'")
    soc_spec = _load_json(soc_path)

    # 4. Board definition.  Two mutually-exclusive sources (the
    # schema's `oneOf` rule enforces this):
    #   - `preset: <name>`  -> load metadata/boards/<name>.yaml
    #   - inline `name:` + `populated:` + `e1m_routes:` at top level
    # Either way the rest of the loader sees a single board_preset
    # dict with `name`, `populated`, `e1m_routes`.
    if "preset" in project:
        board_preset = _resolve_board_preset(
            project["preset"], metadata_root)
    else:
        board_preset = _synthesize_inline_board(project)
    board_name = board_preset.get("name")
    board_hw_rev = (project.get("hw_rev")
                      or board_preset.get("default_hw_rev"))

    # 5. Compute per-core effective mapping.
    project_cores = project.get("cores") or {}
    som_topology = som_preset.get("topology") or {}
    soc_core_ids = [c["id"] for c in (soc_spec.get("cores") or []) if "id" in c]

    # Reject SoM topology keys that don't exist in the SoC spec --
    # surfaces preset bugs early.
    bad_topology = [k for k in som_topology.keys() if k not in soc_core_ids]
    if bad_topology:
        raise OrchestratorError(
            f"SoM preset {sku} `topology:` references core IDs "
            f"{bad_topology} that aren't in SoC {silicon}'s "
            f"cores[] (known: {soc_core_ids})")

    # Phase B gap fix G-4: catch the cross-class `som.sku:` swap where
    # `cores.<key>` doesn't match this SoM preset's `topology:`.
    # Example: customer has `cores.m55_hp:` and swaps som.sku from
    # E1M-AEN701 (topology: m55_hp + m55_he + a32_cluster) to
    # E1M-NX9101 (topology: m33 + a55_cluster).  Pre-fix the slice-
    # build loop iterated topology keys, NOT project_cores keys, so
    # `cores.m55_hp:` was silently dropped and the customer got an
    # empty slice with no diagnostic.  Hard-fail when NO project key
    # matches topology; soft-warn for unmatched keys when SOME match
    # (the customer likely forgot to rename one of several cores).
    # The SoC-level mismatch check (topology is a subset of SoC core
    # IDs per the bad_topology guard above) is subsumed by this
    # topology-level check: anything not in topology is wrong from
    # the customer's POV, whether it's SoC-absent or merely SoM-
    # preset-absent.
    topology_keys = set(som_topology.keys())
    project_keys = set(project_cores.keys())
    unmatched = sorted(project_keys - topology_keys)
    matched = project_keys & topology_keys
    if unmatched and project_keys:
        sku_topology = sorted(topology_keys)
        if not matched:
            raise OrchestratorError(
                f"board.yaml `cores:` declares {unmatched} but the "
                f"SoM SKU {sku}'s `topology:` exposes no such core. "
                f"Did you mean one of: {sku_topology}?")
        for key in unmatched:
            print(
                f"alp_orchestrate: WARN: board.yaml `cores.{key}:` "
                f"has no match in SoM SKU {sku}'s `topology:` "
                f"(exposes: {sku_topology}); slice will be dropped.",
                file=sys.stderr)

    # Index SoC cores[] by id so we can look up `type` for the OS
    # default inference (Finding A: pre-2026-05-18 every SoM YAML's
    # `topology.<core>.os` followed cortex-m* -> zephyr, cortex-a* ->
    # yocto; the loader now infers that fallback when `os:` is absent).
    soc_core_type_by_id: dict[str, str] = {
        str(c["id"]): str(c.get("type") or "")
        for c in (soc_spec.get("cores") or []) if "id" in c
    }

    cores: dict[str, Slice] = {}
    for core_id in soc_core_ids:
        resolved = _resolve_topology_for_core(
            core_id, project_cores, som_topology)
        if resolved is None:
            # Multi-core SoCs require either project cores: or topology
            # preset coverage; single-core SoCs default the one core to
            # the preset (which we already checked above -- if we got
            # here for a single-core SoC, the preset has no topology
            # entry for the only core, which is a real error).
            raise OrchestratorError(
                f"core '{core_id}' has no runtime assigned (neither "
                f"board.yaml `cores.{core_id}` nor SoM preset "
                f"`topology.{core_id}` is set)")
        slice_ = _slice_from_resolved(
            core_id, resolved,
            soc_core_type=soc_core_type_by_id.get(core_id, ""),
        )
        _enforce_loader_rules(slice_)
        _enforce_os_matches_core_class(
            slice_, soc_core_type_by_id.get(core_id, ""))
        cores[core_id] = slice_

    # 6. IPC entries.
    ipc_raw = project.get("ipc") or []
    ipc_entries: list[IpcEntry] = []
    for entry in ipc_raw:
        ipc_entries.append(IpcEntry(
            name=entry["name"],
            kind=entry["kind"],
            endpoints=list(entry["endpoints"]),
            carve_out_kb=int(entry["carve_out_kb"]),
            cacheable=entry.get("cacheable"),
            address=entry.get("address"),
        ))

    # 7. Loader rule §4.5.6: every ipc endpoint must be a core with
    # os != off.
    for e in ipc_entries:
        for ep in e.endpoints:
            if ep not in cores:
                raise OrchestratorError(
                    f"ipc entry '{e.name}' references core '{ep}' "
                    f"that isn't in this project")
            if cores[ep].os == "off":
                raise OrchestratorError(
                    f"ipc entry '{e.name}' references core '{ep}' "
                    f"which is os: off")

    # 8. Optional top-level `pins:` cross-check.  When the project
    # lists which E1M pads it actively uses, every entry must exist
    # in the resolved board's `e1m_routes:` block; entries that
    # supply a `macro:` must also match the board's macro for that
    # pad.  Catches typos + demos drifting from the EVK preset's
    # wiring.  Each entry is either a bare string (just the pad
    # name) or a `{e1m, macro?, doc?}` mapping.
    used_pins = list(project.get("pins") or [])
    if used_pins:
        routes = (board_preset or {}).get("e1m_routes") or {}
        # Build a {pad -> [macros]} index; one pad can have several
        # macros aliasing it (e.g. E1M_PWM1 maps to EVK_PWM_LED_BLUE
        # AND EVK_ARD_PWM1 on the EVK).
        macros_by_pad: dict[str, set[str]] = {}
        for section in ("gpio", "buses", "pwm", "adc", "dac", "i2s", "can", "qenc"):
            for entry in (routes.get(section) or []):
                e1m = entry.get("e1m")
                macro = entry.get("macro")
                if isinstance(e1m, str) and isinstance(macro, str):
                    macros_by_pad.setdefault(e1m, set()).add(macro)
        board_label = board_name or "<inline>"
        for idx, item in enumerate(used_pins):
            if isinstance(item, str):
                e1m_pad, macro_decl = item, None
            elif isinstance(item, dict):
                e1m_pad = item.get("e1m")
                macro_decl = item.get("macro")
            else:
                raise OrchestratorError(
                    f"board.yaml `pins[{idx}]` is neither a string nor a mapping")
            if e1m_pad not in macros_by_pad:
                raise OrchestratorError(
                    f"board.yaml `pins[{idx}].e1m: {e1m_pad}` is not in the "
                    f"resolved board '{board_label}'s `e1m_routes:` block.  "
                    f"Known pads: {sorted(macros_by_pad.keys())}")
            if macro_decl is not None and macro_decl not in macros_by_pad[e1m_pad]:
                raise OrchestratorError(
                    f"board.yaml `pins[{idx}].macro: {macro_decl}` does not "
                    f"match the resolved board '{board_label}'s macros for "
                    f"pad {e1m_pad}: {sorted(macros_by_pad[e1m_pad])}")

    # 9. Storage partitions (board.yaml `storage:` block).  Parse into
    # StorageEntry dataclasses + cross-field check: every `flash_device:`
    # must resolve to either a memory_map region name or an
    # `on_module.ospi_memories:` key.  Resolution of base addresses /
    # overlap detection happens in `resolve_storage_partitions()`; the
    # loader catches typos eagerly because they are cheap to surface
    # before the build kicks off.
    storage_raw = project.get("storage") or []
    storage_entries: list[StorageEntry] = []
    for idx, item in enumerate(storage_raw):
        # `raw: true` is the legacy alias for `fs: raw`; the schema
        # accepts both, the loader normalises.
        fs = item.get("fs")
        if fs is None and item.get("raw") is True:
            fs = "raw"
        if fs is None:
            fs = "raw"
        storage_entries.append(StorageEntry(
            name=item["name"],
            size_kib=int(item["size_kib"]),
            fs=fs,
            mount=item.get("mount"),
            flash_device=item.get("flash_device"),
            offset_kib=(int(item["offset_kib"])
                        if item.get("offset_kib") is not None else None),
        ))

    # Cross-field: known flash device set is memory_map names + ospi keys.
    if storage_entries:
        known_devices = set(_known_flash_devices(som_preset, METADATA_ROOT))
        for entry in storage_entries:
            if entry.flash_device is None:
                continue   # resolver will block it with a clear reason
            if entry.flash_device not in known_devices:
                raise OrchestratorError(
                    f"board.yaml `storage[{entry.name}].flash_device: "
                    f"{entry.flash_device}` does not resolve to any "
                    f"flash device on SoM {sku}.  Known devices: "
                    f"{sorted(known_devices)}")
        # Name uniqueness within storage[].
        names_seen: set[str] = set()
        for entry in storage_entries:
            if entry.name in names_seen:
                raise OrchestratorError(
                    f"board.yaml `storage:` declares partition "
                    f"`{entry.name}` more than once; names must be "
                    f"unique within the project")
            names_seen.add(entry.name)

    # 10. `security.psa:` cross-field validation.  The schema is
    # authoritative on field types; this block enforces the references:
    # ITS/PS storage names must resolve to a `storage[].name`, a SoM
    # memory_map region name, OR an `on_module.ospi_memories:` key
    # (PS-class storage often lives on an on-module OSPI part rather
    # than in MRAM); `attestation_root: optiga_trust_m` requires the
    # SoM to physically ship OPTIGA Trust M.  Errors point at the
    # offending board.yaml path so the customer can fix it.
    security_block = dict(project.get("security") or {})
    psa = dict(security_block.get("psa") or {})
    if psa:
        storage_name_set = {e.name for e in storage_entries}
        try:
            mem_map = resolve_memory_map(som_preset, METADATA_ROOT)
        except Exception:                                # noqa: BLE001
            mem_map = []
        region_names = {
            str(r.get("name")) for r in mem_map
            if isinstance(r, dict) and r.get("name")
        }
        ospi_keys = {
            str(k) for k in
            ((som_preset.get("on_module") or {}).get("ospi_memories") or {}).keys()
            if isinstance(k, str)
        }
        valid_refs = storage_name_set | region_names | ospi_keys

        def _check_backing_store(field: str) -> None:
            ref = psa.get(field)
            if ref is None:
                return
            if str(ref) in valid_refs:
                return
            raise OrchestratorError(
                f"board.yaml `security.psa.{field}: {ref}` does not "
                f"resolve to any `storage[].name`, SoM "
                f"`memory_map[].name`, or `on_module.ospi_memories:` "
                f"key.  Known storage partitions: "
                f"{sorted(storage_name_set) or '[]'}; "
                f"known SoM memory regions: "
                f"{sorted(region_names) or '[]'}; "
                f"known on-module OSPI parts: "
                f"{sorted(ospi_keys) or '[]'}.")

        _check_backing_store("its_storage")
        _check_backing_store("ps_storage")

        att_root = psa.get("attestation_root")
        if att_root == "optiga_trust_m":
            on_module = som_preset.get("on_module") or {}
            chip_set: set[str] = set()
            for key, val in on_module.items():
                if key == "ospi_memories":
                    continue
                if isinstance(val, str):
                    chip_set.add(val)
            capabilities = som_preset.get("capabilities") or {}
            has_optiga = (
                "optiga_trust_m" in chip_set
                or bool(capabilities.get("optiga_trust_m"))
            )
            if not has_optiga:
                raise OrchestratorError(
                    f"board.yaml `security.psa.attestation_root: "
                    f"optiga_trust_m` requires the SoM preset to "
                    f"ship OPTIGA Trust M on-module, but SoM SKU "
                    f"{sku} does not list it under `on_module:` or "
                    f"`capabilities:`.  Pick `tfm_internal` or "
                    f"`none`, or switch to a SoM that carries OPTIGA "
                    f"(AEN family).")

    out = BoardProject(
        sku=sku,
        hw_rev=hw_rev or som_preset.get("default_hw_rev"),
        board_name=board_name,
        board_hw_rev=board_hw_rev,
        cores=cores,
        ipc=ipc_entries,
        soc_spec=soc_spec,
        som_preset=som_preset,
        board_preset=board_preset,
        diagnostics=dict(project.get("diagnostics") or {}),
        chips=list(project.get("chips") or []),
        features=dict(project.get("features") or {}),
        boot=dict(project.get("boot") or {}),
        ota=dict(project.get("ota") or {}),
        storage=storage_entries,
        security=security_block,
        raw=project,
    )

    # 11. Cross-field consistency pass (v0.6 P2.3).  Runs last so it
    # can inspect the fully-assembled project + every per-core
    # extra_libraries: entry the schema couldn't validate cleanly.
    _validate_consistency(out)

    return out


# ---------------------------------------------------------------------
# Carve-out resolver
# ---------------------------------------------------------------------
# resolve_carve_outs lives in carveout.py (the #285 carve-out seam); re-exported
# so the public surface (orchestrator + emitters + tests) stays unchanged.
from .carveout import resolve_carve_outs  # noqa: E402


# ---------------------------------------------------------------------
# Storage partition resolver
# ---------------------------------------------------------------------
# The storage-partition resolver now lives in partition.py (the #285 partition
# seam). Re-export resolve_storage_partitions (emitters + orchestrator + tests)
# and _known_flash_devices (the loader's eager flash-device cross-check above).
from .partition import (  # noqa: E402
    _known_flash_devices,
    resolve_storage_partitions,  # noqa: F401  (re-export for tests)
)


# ---------------------------------------------------------------------
# Emitters
# ---------------------------------------------------------------------


# The header / DTS / mount-table emitters now live in headers.py (the #285
# headers emit seam). Re-exported so cli.py + alp_project.py + the build-plan
# artefact table + tests keep importing them unchanged.
from .headers import (  # noqa: E402
    emit_dts_partitions,  # noqa: F401  (re-export: cli + tests)
    emit_dts_reservations,  # noqa: F401  (re-export: cli + alp_project + tests)
    emit_ipc_contract_h,  # noqa: F401  (re-export: cli + alp_project + tests)
    emit_storage_mounts_c,  # noqa: F401  (re-export: cli + alp_project + tests; not used in __init__)
)


# emit_system_manifest + its helper_mcus assembler now live in manifest.py (the
# #285 manifest emit seam). Re-export both: emit_system_manifest for cli +
# alp_project + tests + the Orchestrator's materialise path; _helper_mcus is
# back-imported because the Orchestrator (still inline) also assembles it.
from .manifest import _helper_mcus, emit_system_manifest  # noqa: E402


# ---------------------------------------------------------------------
# Build-plan emission (the Wave C consumer contract)
# ---------------------------------------------------------------------


# The build-plan emitter now lives in buildplan.py (the #285 build-plan seam).
# Re-export emit_build_plan (cli + tests) + back-import the shared materialise
# helpers the Orchestrator below reads (byte-parity contract with the plan).
from .buildplan import (  # noqa: E402
    _shared_artefacts,
    _slice_build_dir,
    _slice_config_artefact,
    emit_build_plan,  # noqa: F401  (re-export for cli + tests; not called in __init__)
)


# ---------------------------------------------------------------------
# Orchestrator (fan-out)
# ---------------------------------------------------------------------


# Tool table used to decide whether a slice can actually be built on
# this host.  Each os maps to the executable the slice's build dispatch
# needs; missing tools land the slice in `status: skipped`.
_TOOL_FOR_OS: dict[str, str] = {
    "zephyr":    "west",
    "yocto":     "bitbake",
    "baremetal": "cmake",
    # 'off' never reaches the dispatcher.
}


class Orchestrator:
    """Fans out one build sub-process per non-off slice.

    Phase 2 ships the dispatch + manifest assembly; the per-os build
    invocations are stubbed where a tool isn't present so the
    orchestrator completes end-to-end on Windows / non-Yocto hosts.
    """

    def __init__(
        self,
        project: BoardProject,
        build_root: Path,
    ) -> None:
        self.project = project
        self.build_root = Path(build_root)
        self.state_path = self.build_root / ".alp-build-state.json"
        self._state: dict[str, Any] = self._load_state()

    # ---- state cache ----

    def _load_state(self) -> dict[str, Any]:
        if not self.state_path.is_file():
            return {}
        try:
            return json.loads(self.state_path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            return {}

    def _save_state(self) -> None:
        try:
            self.state_path.parent.mkdir(parents=True, exist_ok=True)
            self.state_path.write_text(
                json.dumps(self._state, indent=2, sort_keys=True),
                encoding="utf-8")
        except OSError as e:
            print(f"alp-orchestrate: warning: failed to write "
                  f"{self.state_path}: {e}", file=sys.stderr)

    def _slice_hash(self, slice_: Slice) -> str:
        """Hash the inputs that determine a slice's output."""
        import hashlib
        m = hashlib.sha256()
        m.update(self.project.sku.encode("utf-8"))
        m.update(slice_.os.encode("utf-8"))
        m.update((slice_.app or "").encode("utf-8"))
        m.update((slice_.image or "").encode("utf-8"))
        m.update((slice_.board or "").encode("utf-8"))
        m.update((slice_.machine or "").encode("utf-8"))
        m.update(",".join(sorted(slice_.peripherals)).encode("utf-8"))
        m.update(",".join(sorted(slice_.libraries)).encode("utf-8"))
        m.update(json.dumps(slice_.inference, sort_keys=True)
                 .encode("utf-8"))
        m.update(json.dumps(slice_.iot, sort_keys=True)
                 .encode("utf-8"))
        for entry in sorted(self.project.ipc, key=lambda e: e.name):
            m.update(entry.name.encode("utf-8"))
            m.update(entry.kind.encode("utf-8"))
            m.update(",".join(sorted(entry.endpoints)).encode("utf-8"))
            m.update(str(entry.carve_out_kb).encode("utf-8"))
        return m.hexdigest()[:16]

    # ---- materialisation ----

    def _materialise_slice_config(self, slice_: Slice) -> Path:
        """Write per-core config artefacts under
        build/<core>-<os>/.

        The artefact itself comes from `_slice_config_artefact` -- the
        same source `emit_build_plan` reads -- so what we write and
        what the plan promises cannot drift.
        """
        slice_dir = _slice_build_dir(self.build_root, slice_)
        slice_dir.mkdir(parents=True, exist_ok=True)
        slice_.build_dir = slice_dir
        slice_.log_path = slice_dir / "build.log"
        artefact = _slice_config_artefact(self.project, slice_)
        if artefact is not None:
            name, contents = artefact
            (slice_dir / name).write_text(contents, encoding="utf-8")
        return slice_dir

    def _materialise_shared(self) -> Path:
        """Write the shared generated artefacts.

        The (path, contents) set comes from `_shared_artefacts` -- the
        same source `emit_build_plan` reads -- so what we write and
        what the plan promises cannot drift.  Conditional artefacts
        (sysbuild / TF-M) are absent from the list when empty, so
        their directories are never created spuriously.
        """
        gen = self.build_root / "generated"
        for path, contents in _shared_artefacts(self.project,
                                                self.build_root):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")
        return gen

    # ---- dispatch ----

    def _dispatch_slice(self, slice_: Slice) -> Slice:
        """Run the per-slice build sub-process (or skip if its tool
        isn't on PATH)."""
        if slice_.os == "off":
            slice_.status = "skipped"
            slice_.reason = "os: off"
            return slice_

        tool = _TOOL_FOR_OS.get(slice_.os)
        if tool is None:
            slice_.status = "failed"
            slice_.reason = f"unknown os '{slice_.os}'"
            return slice_

        if shutil.which(tool) is None:
            slice_.status = "skipped"
            slice_.reason = (f"{tool} not found in PATH; this is normal "
                             f"on non-{slice_.os} dev hosts")
            return slice_

        cmd = _slice_command(self.project, slice_)
        if cmd is None:
            slice_.status = "skipped"
            if slice_.os == "zephyr" and slice_.app == STOCK_SHIM_APP:
                slice_.reason = (
                    f"stock M-core shim (app: {STOCK_SHIM_APP}) -- image body "
                    f"not in the SDK tree yet (issue #49); override "
                    f"cores.{slice_.core_id}.app to build this core")
            else:
                slice_.reason = ("no command resolver implemented yet for "
                                 f"os: {slice_.os} -- Phase 3 wires this up")
            return slice_

        # Slice subprocess: scoped env + dedicated log file.
        env = os.environ.copy()
        env["ALP_SDK_ROOT"] = str(REPO)
        log_path = slice_.log_path or (slice_.build_dir / "build.log")
        start = time.time()
        try:
            with open(log_path, "w", encoding="utf-8") as logf:
                logf.write(f"# alp_orchestrate.py slice command: "
                           f"{' '.join(cmd)}\n")
                logf.flush()
                proc = subprocess.run(
                    cmd, cwd=str(slice_.build_dir),
                    env=env, stdout=logf, stderr=subprocess.STDOUT,
                    check=False)
            slice_.duration_s = time.time() - start
            if proc.returncode == 0:
                slice_.status = "ok"
            else:
                slice_.status = "failed"
                slice_.reason = (f"{tool} exited rc={proc.returncode}; "
                                 f"see {log_path}")
        except (OSError, subprocess.SubprocessError) as e:
            slice_.duration_s = time.time() - start
            slice_.status = "failed"
            slice_.reason = f"slice subprocess raised: {e}"
        return slice_

    # ---- public API ----

    def fan_out(
        self,
        only_core: Optional[str] = None,
        parallel: bool = True,
    ) -> SystemManifest:
        """Run every non-off slice, write the manifest, return it."""
        self.build_root.mkdir(parents=True, exist_ok=True)
        # 1. Shared artefacts.
        self._materialise_shared()

        # 2. Per-slice config materialisation.
        targets: list[Slice] = []
        for cid, slice_ in self.project.cores.items():
            if slice_.os == "off":
                slice_.status = "skipped"
                slice_.reason = "os: off"
                continue
            if only_core is not None and cid != only_core:
                slice_.status = "skipped"
                slice_.reason = f"--core {only_core} selected; this slice not in scope"
                continue
            self._materialise_slice_config(slice_)
            targets.append(slice_)

        # 3. Caching: skip slices whose inputs hash matches the last
        #    successful run AND whose build dir still exists.
        skip_targets: set[str] = set()
        for slice_ in targets:
            h = self._slice_hash(slice_)
            cached = self._state.get(slice_.core_id) or {}
            if (cached.get("hash") == h
                    and cached.get("status") == "ok"
                    and slice_.build_dir and slice_.build_dir.is_dir()):
                slice_.status = "ok"
                slice_.reason = "cache-hit (inputs unchanged since last successful build)"
                slice_.output_artefact = cached.get("output_artefact")
                skip_targets.add(slice_.core_id)

        runnable = [s for s in targets if s.core_id not in skip_targets]

        # 4. Dispatch.  Use ProcessPoolExecutor for parallel, but the
        # cheap reality on Phase 2 is most slices skip (missing tool)
        # so the sequential path is fine when parallel=False.
        if parallel and len(runnable) > 1:
            try:
                from concurrent.futures import ProcessPoolExecutor, as_completed
                with ProcessPoolExecutor(max_workers=len(runnable)) as ex:
                    futures = {
                        ex.submit(self._dispatch_slice, s): s
                        for s in runnable
                    }
                    for fut in as_completed(futures):
                        result = fut.result()
                        # Mutate the original Slice in place.
                        original = futures[fut]
                        original.status = result.status
                        original.reason = result.reason
                        original.duration_s = result.duration_s
                        original.output_artefact = result.output_artefact
            except (OSError, RuntimeError) as e:
                # ProcessPool on Windows in some envs fails; degrade to
                # sequential.
                print(f"alp-orchestrate: ProcessPoolExecutor unusable "
                      f"({e}); falling back to sequential", file=sys.stderr)
                for slice_ in runnable:
                    self._dispatch_slice(slice_)
        else:
            for slice_ in runnable:
                self._dispatch_slice(slice_)

        # 5. Persist cache state.
        for slice_ in self.project.cores.values():
            if slice_.status == "ok":
                self._state[slice_.core_id] = {
                    "hash":           self._slice_hash(slice_),
                    "status":         slice_.status,
                    "output_artefact": slice_.output_artefact,
                }
        self._save_state()

        # 6. Manifest.
        ordered = sorted(self.project.cores.values(),
                         key=lambda s: s.core_id)
        manifest = SystemManifest(
            project=self.project,
            slices=ordered,
            carve_outs=resolve_carve_outs(self.project),
            boot_order=list(self.project.som_preset.get("boot_order") or []),
            helper_mcus=_helper_mcus(self.project),
        )

        out = self.build_root / "system-manifest.yaml"
        out.write_text(emit_system_manifest(
            self.project, slices=ordered), encoding="utf-8")

        return manifest


# ---------------------------------------------------------------------
# Per-slice config emitters (consumed by both Orchestrator and the
# new --core <id> --emit zephyr-conf / yocto-conf modes in alp_project.py).
# ---------------------------------------------------------------------


# The shared slug / peripheral-Kconfig helpers now live in slugs.py (the #285
# slug leaf). Re-exported for the per-slice config emitters below + the tests.
from .slugs import (  # noqa: E402
    _slugs_from_helper_firmware,  # noqa: F401  (re-export for tests)
    _slugs_from_on_module,  # noqa: F401  (re-export for tests)
)


# The per-slice Kconfig (alp.conf) emitter now lives in kconfig.py (the #285
# kconfig emit seam); it pulls the slug tables from slugs.py. Re-exported for
# the build-plan _slice_config_artefact helper (inline) + alp_project + tests.
from .kconfig import (  # noqa: E402
    _emit_extra_library_profile,  # noqa: F401  (re-export for tests)
    _slice_alp_conf,  # noqa: F401  (re-export: alp_project + tests)
    _slice_cmake_args,  # noqa: F401  (re-export: alp_project + tests)
    _slice_local_conf,  # noqa: F401  (re-export: alp_project + tests)
)


# The sysbuild / TF-M secure-boot config emitters now live in secure.py (the
# #285 secure emit seam). Re-exported so cli.py + tests + the build-plan artefact
# table / slice-command helpers (still inline) keep importing them unchanged.
from .secure import emit_sysbuild_conf, emit_tfm_sysbuild_conf  # noqa: E402


def _slice_flash_recipe(
    slice_: Slice,
) -> tuple[Optional[str], Optional[dict[str, Any]]]:
    """Per-runtime default flash backend + args for a slice.

    Used by `Slice.to_manifest_entry` to record how `west alp-flash`
    should program the slice's output artefact.  The actual backend
    implementations land in subsequent PRs alongside `alp_flash.py`
    -- this Phase 3 wiring is just data plumbing.

    Returns ``(None, None)`` for `os: off` slices (skipped at flash
    time) and unknown `os:` values; the manifest emitter drops keys
    with None values, so off slices stay tidy.
    """
    if slice_.os == "yocto":
        return ("yocto_wic_to_sd_or_emmc",
                {"target": slice_.machine or ""})
    if slice_.os == "zephyr":
        # OpenOCD is the canonical Zephyr runner for the SoCs we
        # ship; vendor-specific runners (jlink for AEN, segger for
        # NX) are picked up via the slice's toolchain when set.
        runner = "openocd"
        return ("zephyr_west_flash", {"runner": runner})
    if slice_.os == "baremetal":
        return ("baremetal_cmake_flash", {})
    return (None, None)


# Placeholder M-core "stock shim" app token (Zephyr side).  Accepted by the
# SoM-preset schema and defaulted into M-core slots (AEN m55_hp/he, V2N
# m33_sm, NX91 m33), but the shim image body is not yet in the SDK tree
# (issue #49), so it resolves to no build command.
STOCK_SHIM_APP = "alp-stock-shim"


def _slice_command(
    project: BoardProject,
    slice_: Slice,
) -> Optional[list[str]]:
    """Resolve the build command for a slice.  Returns None when there is no
    buildable command yet -- the caller carries the slice as `skipped` /
    `no-command`, never dropped.  This includes the stock M-core shim
    (`app: alp-stock-shim`), whose image body isn't in the SDK tree yet
    (issue #49)."""
    if slice_.os == "zephyr":
        if not slice_.app or not slice_.board:
            return None
        # The stock shim is a placeholder token, not a buildable app dir;
        # resolve no command rather than west-building a path that doesn't
        # exist.  Override cores.<id>.app with a real app to build the core.
        if slice_.app == STOCK_SHIM_APP:
            return None
        cmd = [
            "west", "build",
            "-b", slice_.board,
            str(_zephyr_app_dir(slice_.app)),
        ]
        # ADR 0014 Phase-3 conf->build: wire the generated sysbuild
        # overlays into the build command itself.  `_shared_artefacts`
        # emits the top-level overlay at build_root/alp_sysbuild.conf and
        # the TF-M child overlay at build_root/sysbuild/tfm/tfm.conf; the
        # command runs with cwd=build_dir (build/<core>-<os>), so the
        # top-level overlay is one directory up.  Pass --sysbuild whenever
        # a sysbuild child image is configured (a `boot:` or `security.psa:`
        # block), and --sysbuild-config only when the top-level overlay is
        # non-empty (the TF-M overlay is picked up by sysbuild convention
        # from its sysbuild/tfm/ path).  Absent both, the stock per-family
        # sysbuild defaults apply and no flag is added.
        if emit_sysbuild_conf(project) or emit_tfm_sysbuild_conf(project):
            cmd.append("--sysbuild")
            if emit_sysbuild_conf(project):
                cmd += ["--sysbuild-config", "../alp_sysbuild.conf"]
        return cmd
    if slice_.os == "yocto":
        target = slice_.image or slice_.app
        if not target:
            return None
        return ["bitbake", str(target)]
    if slice_.os == "baremetal":
        if not slice_.app:
            return None
        return ["cmake", "-S", str(_resolve_app_path(slice_.app)),
                "-B", str(slice_.build_dir)]
    return None


def _resolve_app_path(app: str) -> Path:
    """Resolve `./linux` or absolute paths from a slice.app."""
    p = Path(app)
    if p.is_absolute():
        return p
    return (Path.cwd() / p).resolve()


def _zephyr_app_dir(app: str) -> Path:
    """Resolve a Zephyr slice's `app:` to the directory holding the
    application `CMakeLists.txt` (what `west build` needs).

    Two example conventions are supported:

      * multicore examples point `app:` straight at a self-contained
        Zephyr app directory (e.g. ``./m33_sm`` -- carries its own
        CMakeLists.txt + prj.conf); used verbatim.
      * single-core examples keep one CMakeLists.txt at the example
        root and point `app:` at the sources subdir (e.g. ``./src`` with
        ``target_sources(app PRIVATE src/main.c)``).  The sources dir has
        no CMakeLists.txt of its own, so fall back to its parent (the
        example root) which does.
    """
    p = _resolve_app_path(app)
    if (p / "CMakeLists.txt").is_file():
        return p
    if (p.parent / "CMakeLists.txt").is_file():
        return p.parent
    return p


# ---------------------------------------------------------------------
# CLI (thin wrapper for ad-hoc invocation)
# ---------------------------------------------------------------------


# Re-export the CLI entry: main() lives in __main__ (so `python -m
# alp_orchestrate` is the invocation), but `from alp_orchestrate import main`
# stays valid for callers + the test-suite.  Placed at module end so __main__'s
# `from alp_orchestrate import ...` sees a fully-populated package.
from .cli import main  # noqa: F401,E402  (intentional re-export)
