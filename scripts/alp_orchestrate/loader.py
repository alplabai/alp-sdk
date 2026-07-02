#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""board.yaml loader -- file IO, preset/silicon resolution, and load_board_yaml.

Parses board.yaml into a BoardProject: YAML/JSON IO, schema validation (via the
one shared alp_cli.validator), the `preset:` shared-board + inline-board
resolution, silicon-ref -> SoC JSON path, per-core topology defaults, and the
big `load_board_yaml` entry point (which finishes by running the cross-field
validators). Extracted as the #285 loader seam.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any, Optional

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("alp_orchestrate: PyYAML is required.  Install via `pip install pyyaml`.")

try:
    import jsonschema  # type: ignore[import-untyped]  # noqa: F401  (dep gate)
except ImportError:
    sys.exit("alp_orchestrate: jsonschema is required.  Install via `pip install jsonschema`.")

from alp_cli.validator import iter_schema_errors
from alp_project import resolve_memory_map

from .models import BoardProject, IpcEntry, OrchestratorError, Slice, StorageEntry
from .partition import _known_flash_devices
from .paths import BOARD_SCHEMA, METADATA_ROOT, REPO
from .topology import _default_os_from_core_type
from .validate import (
    _enforce_loader_rules,
    _enforce_os_matches_core_class,
    _validate_consistency,
)


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
    errors = iter_schema_errors(project, schema_path)
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
