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


def _check_board_hosts_som_family(
    sku: str,
    som_preset: dict[str, Any],
    preset: str,
    board_preset: dict[str, Any],
) -> None:
    family = som_preset.get("family")
    allowed_raw = board_preset.get("hosts_som_families")
    if not isinstance(family, str) or not isinstance(allowed_raw, list):
        return
    allowed = [str(item) for item in allowed_raw]
    if family in allowed:
        return
    raise OrchestratorError(
        f"board preset '{preset}' hosts SoM families {allowed}, "
        f"but {sku} is family '{family}'")


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
        recipe=entry.get("recipe"),
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
        # Absent -> True (the core has a HW console); only a SoM preset's
        # `topology.<id>.hw_console: false` marks a headless core.
        hw_console=bool(entry.get("hw_console", True)),
    )


def _load_and_validate_yaml(path: Path,
                            metadata_root: Path) -> dict[str, Any]:
    """Stage 1 of the #673 Phase-1 `load_board_yaml` split: read
    board.yaml and run schema validation.

    Pass metadata_root so test stubs using non-production SKU patterns
    (E1M-TST001 etc.) validate against their own copy of the schema
    rather than the repo's strict pattern.
    """
    path = Path(path)
    project = _load_yaml(path)
    _validate_board(project, metadata_root=metadata_root)
    return project


def _resolve_board(
    project: dict[str, Any],
    metadata_root: Path,
) -> tuple[str, Optional[str], dict[str, Any], str, dict[str, Any],
           dict[str, Any], Optional[str], Optional[str]]:
    """Stage 2 of the #673 Phase-1 `load_board_yaml` split: SoM SKU
    preset, SoC spec, and board (preset or inline) resolution.

    Returns (sku, hw_rev, som_preset, silicon, soc_spec, board_preset,
    board_name, board_hw_rev).
    """
    sku = project["som"]["sku"]
    hw_rev = project["som"].get("hw_rev")

    # Resolve SKU preset.
    sku_preset_path = metadata_root / "e1m_modules" / f"{sku}.yaml"
    if not sku_preset_path.is_file():
        raise OrchestratorError(
            f"no preset for SoM SKU {sku} at "
            f"{sku_preset_path.relative_to(REPO) if sku_preset_path.is_relative_to(REPO) else sku_preset_path}")
    som_preset = _load_yaml(sku_preset_path)

    # Resolve SoC spec via the preset's `silicon:` ref.
    silicon = som_preset.get("silicon")
    if not silicon:
        raise OrchestratorError(
            f"SoM preset {sku} has no `silicon:` field")
    soc_path = _silicon_to_soc_path(silicon, metadata_root)
    if not soc_path.is_file():
        raise OrchestratorError(
            f"no SoC spec at {soc_path.relative_to(REPO) if soc_path.is_relative_to(REPO) else soc_path} for ref '{silicon}'")
    soc_spec = _load_json(soc_path)

    # Board definition.  Two mutually-exclusive sources (the
    # schema's `oneOf` rule enforces this):
    #   - `preset: <name>`  -> load metadata/boards/<name>.yaml
    #   - inline `name:` + `populated:` + `e1m_routes:` at top level
    # Either way the rest of the loader sees a single board_preset
    # dict with `name`, `populated`, `e1m_routes`.
    if "preset" in project:
        board_preset = _resolve_board_preset(project["preset"], metadata_root)
        _check_board_hosts_som_family(
            sku, som_preset, project["preset"], board_preset)
    else:
        board_preset = _synthesize_inline_board(project)
    board_name = board_preset.get("name")
    board_hw_rev = (project.get("hw_rev")
                      or board_preset.get("default_hw_rev"))

    return (sku, hw_rev, som_preset, silicon, soc_spec, board_preset,
            board_name, board_hw_rev)


def _validate_topology_cores(
    project: dict[str, Any],
    som_preset: dict[str, Any],
    soc_spec: dict[str, Any],
    sku: str,
    silicon: str,
    board_preset: dict[str, Any],
    board_name: Optional[str],
) -> tuple[dict[str, Slice], list[IpcEntry]]:
    """Stage 3 of the #673 Phase-1 `load_board_yaml` split: per-core
    topology resolution + OS/class enforcement, IPC endpoint
    cross-checks, and the optional top-level `pins:` cross-check
    against the resolved board's `e1m_routes:`.

    Returns (cores, ipc_entries).
    """
    # Compute per-core effective mapping.
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

    # Phase B gap fix G-4 (hardened by #603): catch the cross-class
    # `som.sku:` swap where `cores.<key>` doesn't match this SoM preset's
    # `topology:`.  Example: customer has `cores.m55_hp:` and swaps
    # som.sku from E1M-AEN801 (topology: m55_hp + m55_he + a32_cluster)
    # to E1M-NX9101 (topology: m33 + a55_cluster).  Pre-fix the slice-
    # build loop iterated topology keys, NOT project_cores keys, so
    # `cores.m55_hp:` was silently dropped and the customer got an
    # empty slice with no diagnostic.
    #
    # #603: EVERY unmatched key under `cores:` is a hard error, not just
    # the all-unmatched case -- a `cores:` mapping with one valid core
    # and one typo used to only warn-and-drop the typo, so a misspelled
    # core silently vanished from the build while the file still
    # validated "clean".  There is no compatibility policy that
    # tolerates an unknown core key, so this is unconditional.  The
    # SoC-level mismatch check (topology is a subset of SoC core IDs
    # per the bad_topology guard above) is subsumed by this topology-
    # level check: anything not in topology is wrong from the
    # customer's POV, whether it's SoC-absent or merely SoM-preset-
    # absent.
    topology_keys = set(som_topology.keys())
    project_keys = set(project_cores.keys())
    unmatched = sorted(project_keys - topology_keys)
    if unmatched:
        sku_topology = sorted(topology_keys)
        raise OrchestratorError(
            f"board.yaml `cores:` declares unknown core id(s) {unmatched} "
            f"that {sku}'s `topology:` does not expose. "
            f"Did you mean one of: {sku_topology}?")

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

    # IPC entries.
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

    # Loader rule §4.5.6: every ipc endpoint must be a core with
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

    # Optional top-level `pins:` cross-check.  When the project
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

    return cores, ipc_entries


def _resolve_storage(
    project: dict[str, Any],
    som_preset: dict[str, Any],
    sku: str,
) -> list[StorageEntry]:
    """Stage 4 of the #673 Phase-1 `load_board_yaml` split: storage
    partitions (board.yaml `storage:` block).  Parse into StorageEntry
    dataclasses + cross-field check: every `flash_device:` must resolve
    to either a memory_map region name or an `on_module.ospi_memories:`
    key.  Resolution of base addresses / overlap detection happens in
    `resolve_storage_partitions()`; the loader catches typos eagerly
    because they are cheap to surface before the build kicks off.
    """
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

    return storage_entries


def _validate_cross_fields(
    project: dict[str, Any],
    som_preset: dict[str, Any],
    sku: str,
    storage_entries: list[StorageEntry],
) -> dict[str, Any]:
    """Stage 5 of the #673 Phase-1 `load_board_yaml` split:
    `security.psa:` cross-field validation.  The schema is
    authoritative on field types; this block enforces the references:
    ITS/PS storage names must resolve to a `storage[].name`, a SoM
    memory_map region name, OR an `on_module.ospi_memories:` key
    (PS-class storage often lives on an on-module OSPI part rather
    than in MRAM); `attestation_root: optiga_trust_m` requires the
    SoM to physically ship OPTIGA Trust M.  Errors point at the
    offending board.yaml path so the customer can fix it.

    Returns the raw `security:` block for BoardProject assembly.
    """
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

    return security_block


def _library_alias_table(metadata_root: Path) -> dict[str, str]:
    """Legacy per-core `libraries:` token -> canonical manifest name
    (metadata/library-aliases-v1.json).  Empty dict if the table is absent."""
    path = metadata_root / "library-aliases-v1.json"
    if not path.is_file():
        return {}
    doc = json.loads(path.read_text(encoding="utf-8"))
    aliases = doc.get("aliases")
    return dict(aliases) if isinstance(aliases, dict) else {}


def _normalize_libraries(project: dict[str, Any],
                         metadata_root: Path) -> None:
    """Fold the unified top-level `libraries:` list into the internal channels
    the emitters consume (WS6-c #610 §6).

    board.yaml declares every curated library once, at the top level, as a
    `{name, cores?}` object: project-wide when `cores:` is omitted, core-scoped
    otherwise (a bare string is accepted as shorthand for a project-wide
    `{name}`).  This rewrites the parsed dict so the rest of the loader stays
    library-shape-agnostic: project-wide names land in `project['libraries']`
    and core-scoped names are injected into each `cores[<id>]['libraries']`.

    This is the ONLY library read path -- there is no separate per-core
    `cores.<id>.libraries:` list to read.
    """
    unified = project.get("libraries") or []
    alias = _library_alias_table(metadata_root)
    cores_map = project.get("cores") or {}
    project_wide: list[str] = []
    per_core: dict[str, list[str]] = {}
    for entry in unified:
        if isinstance(entry, str):
            project_wide.append(alias.get(entry, entry))
            continue
        if not isinstance(entry, dict):
            continue
        name = entry.get("name")
        if not name:
            continue
        canonical = alias.get(name, name)
        cores = entry.get("cores")
        if cores:
            for cid in cores:
                per_core.setdefault(str(cid), []).append(canonical)
        else:
            project_wide.append(canonical)
    project["libraries"] = project_wide
    for cid, names in per_core.items():
        centry = cores_map.get(cid)
        if not isinstance(centry, dict):
            continue
        existing = list(centry.get("libraries") or [])
        for n in names:
            if n not in existing:
                existing.append(n)
        centry["libraries"] = existing


def load_board_yaml(path: Path, *,
                    metadata_root: Path = METADATA_ROOT) -> BoardProject:
    """Load + validate a board.yaml.

    Raises OrchestratorError on any schema / preset / topology error.

    #673 Phase 1: staged into a resolve pipeline.  YAML/schema load,
    board/SKU resolution, topology/core validation, storage resolution,
    and final cross-field validation each run as their own private
    helper (`_load_and_validate_yaml`, `_resolve_board`,
    `_validate_topology_cores`, `_resolve_storage`,
    `_validate_cross_fields`), invoked in the exact same order as the
    original monolithic function so error precedence and messages are
    unchanged.
    """
    project = _load_and_validate_yaml(path, metadata_root)

    # Fold the unified top-level `libraries:` list into the per-core /
    # project-wide channels the downstream resolution expects, so topology +
    # slice building stay library-shape-agnostic.
    _normalize_libraries(project, metadata_root)

    (sku, hw_rev, som_preset, silicon, soc_spec, board_preset,
     board_name, board_hw_rev) = _resolve_board(project, metadata_root)

    cores, ipc_entries = _validate_topology_cores(
        project, som_preset, soc_spec, sku, silicon, board_preset,
        board_name)

    storage_entries = _resolve_storage(project, som_preset, sku)

    security_block = _validate_cross_fields(
        project, som_preset, sku, storage_entries)

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
        libraries=list(project.get("libraries") or []),
        features=dict(project.get("features") or {}),
        boot=dict(project.get("boot") or {}),
        ota=dict(project.get("ota") or {}),
        storage=storage_entries,
        security=security_block,
        raw=project,
    )

    # Cross-field consistency pass (v0.6 P2.3).  Runs last so it can
    # inspect the fully-assembled project + every per-core
    # extra_libraries: entry the schema couldn't validate cleanly.
    _validate_consistency(out)

    return out
