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
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Optional

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
from alp_project import resolve_memory_map, resolve_capabilities  # noqa: E402


REPO = Path(__file__).resolve().parent.parent
METADATA_ROOT = REPO / "metadata"
BOARD_SCHEMA = METADATA_ROOT / "schemas" / "board.schema.json"
BOARD_PRESET_SCHEMA = METADATA_ROOT / "schemas" / "board-preset.schema.json"


def _default_os_from_core_type(core_type: str) -> str:
    """Infer default OS from a SoC's `cores[].type`.

    Convention (codified across the SoM presets pre-2026-05-18):
        cortex-a*  ->  yocto
        cortex-m*  ->  zephyr
        anything else ->  off

    Used as the fallback when a SoM preset's `topology.<core>.os` is
    omitted (the field is now optional in som-preset-v1.schema.json --
    M-class cores default to Zephyr, A-class to Yocto).
    """
    t = (core_type or "").lower()
    if t.startswith("cortex-a"):
        return "yocto"
    if t.startswith("cortex-m"):
        return "zephyr"
    return "off"


class OrchestratorError(RuntimeError):
    """Raised when the orchestrator can't resolve / build a project.

    Carries a human-readable message; the caller (west wrapper / CI)
    prints it and exits non-zero.
    """


# ---------------------------------------------------------------------
# Dataclasses
# ---------------------------------------------------------------------


@dataclass
class Slice:
    """One per-core build slice."""

    core_id: str
    os: str                              # zephyr | yocto | baremetal | off
    app: Optional[str] = None
    image: Optional[str] = None          # Yocto image recipe name
    machine: Optional[str] = None        # Yocto MACHINE
    board: Optional[str] = None          # Zephyr board target
    toolchain: Optional[str] = None
    peripherals: list[str] = field(default_factory=list)
    libraries: list[str] = field(default_factory=list)
    # Open-set escape hatch for libraries the SDK doesn't curate.  Each
    # entry is a dict with `name:` + (exclusively) `kconfig:` OR
    # `profile:`; loader's _validate_consistency() enforces the
    # exactly-one and uniqueness rules.  See docs/board-config.md
    # `extra_libraries:`.
    extra_libraries: list[dict[str, Any]] = field(default_factory=list)
    inference: dict[str, Any] = field(default_factory=dict)
    iot: dict[str, Any] = field(default_factory=dict)
    memory: dict[str, Any] = field(default_factory=dict)   # stack_kib, heap_kib, isr_stack_kib
    power: dict[str, Any] = field(default_factory=dict)    # sleep_mode, wakeup_sources

    # Populated by Orchestrator.fan_out:
    build_dir: Optional[Path] = None
    output_artefact: Optional[str] = None
    status: str = "pending"              # pending | ok | failed | skipped
    reason: Optional[str] = None         # populated for skipped / failed
    log_path: Optional[Path] = None
    duration_s: float = 0.0

    def to_manifest_entry(self) -> dict[str, Any]:
        """Project this slice as a dict for system-manifest.yaml.

        Includes the per-os `flash_method:` + `flash_args:` so
        `west alp-flash` can dispatch each slice without re-deriving
        the backend.  The actual backend implementations (driver
        invocations) are the subject of Phase 5 follow-ups; this
        Phase 3 wiring is just the data plumbing.

        Spec §6.1 byte-stability: the manifest MUST be deterministic
        across rebuilds.  `duration_s` is a wall-clock runtime metric
        that varies run-to-run, so it stays on the Slice dataclass
        but never lands in the manifest.  Same goes for anything else
        timer / PID-style — keep the manifest content-addressable.
        """
        flash_method, flash_args = _slice_flash_recipe(self)
        entry: dict[str, Any] = {
            "core_id":          self.core_id,
            "os":               self.os,
            "app":              self.app,
            "image":            self.image,
            "machine":          self.machine,
            "board":            self.board,
            "toolchain":        self.toolchain,
            "build_dir":        str(self.build_dir) if self.build_dir else None,
            "output_artefact":  self.output_artefact,
            "status":           self.status,
            "log_path":         str(self.log_path) if self.log_path else None,
            "flash_method":     flash_method,
            "flash_args":       flash_args,
        }
        if self.reason:
            entry["reason"] = self.reason
        # Drop keys with None values to keep the manifest tidy.
        return {k: v for k, v in entry.items() if v is not None}


@dataclass
class IpcEntry:
    """Raw IPC declaration straight from board.yaml."""

    name: str
    kind: str
    endpoints: list[str]
    carve_out_kb: int
    cacheable: Optional[bool] = None
    address: Optional[int] = None    # explicit base-address override


@dataclass
class ResolvedCarveOut:
    """An IpcEntry after allocation from the SoM memory_map.

    `status` is "ok" for a fully resolved carve-out; "blocked" when
    the SoM metadata has TBDs (mailbox controller, memory_map base /
    size, etc.) or the board.yaml entry can't be satisfied.  Blocked
    entries land in `system-manifest.yaml` with `status: blocked` +
    `reason: ...` so reviewers see the gap; the actual slice-build
    step (which CI doesn't run) is what fails on a blocked carve-out.
    """

    name: str
    kind: str
    endpoints: list[str]
    base: int                # 0 when blocked
    size: int                # in bytes; 0 when blocked
    region: str              # source memory-region name; "" when blocked
    cacheable: bool
    src_ept: int             # 0 when blocked
    dst_ept: int             # 0 when blocked
    mailbox_channel: int     # 0 when blocked
    status: str = "ok"       # "ok" | "blocked"
    reason: Optional[str] = None     # populated when blocked

    def to_manifest_entry(self) -> dict[str, Any]:
        if self.status == "blocked":
            return {
                "name":      self.name,
                "kind":      self.kind,
                "endpoints": list(self.endpoints),
                "status":    "blocked",
                "reason":    self.reason or "",
            }
        return {
            "name":            self.name,
            "kind":            self.kind,
            "endpoints":       list(self.endpoints),
            "carve_out_base":  f"0x{self.base:08x}",
            "carve_out_size":  f"0x{self.size:08x}",
            "carve_out_region": self.region,
            "cacheable":       self.cacheable,
            "rpmsg_endpoint_ids": {
                "src": f"0x{self.src_ept:08x}",
                "dst": f"0x{self.dst_ept:08x}",
            },
            "mailbox_channel": self.mailbox_channel,
        }


@dataclass
class StorageEntry:
    """Raw storage-partition declaration straight from board.yaml.

    Mirrors the shape under `storage:` in board.schema.json; the
    orchestrator turns these into ResolvedPartitions in
    `resolve_storage_partitions()`.
    """

    name: str
    size_kib: int
    fs: str                  # littlefs | fat | ext4 | raw
    mount: Optional[str] = None
    flash_device: Optional[str] = None
    offset_kib: Optional[int] = None     # explicit offset override


@dataclass
class ResolvedPartition:
    """A StorageEntry after allocation against the SoM flash devices.

    `status` follows the IPC carve-out convention: "ok" for a fully
    resolved partition; "blocked" when the SoM metadata has TBDs
    (flash device base/size unset) or the entry can't be satisfied
    (unknown flash_device, page-misaligned offset, overlap with a
    sibling partition).  Blocked entries land in `system-manifest.yaml`
    with `reason: ...` so reviewers see the gap.
    """

    name: str
    fs: str
    flash_device: str        # original SDK name from board.yaml
    dt_label: str            # Zephyr DT label resolved by the loader
    base_kib: int            # offset within the flash device, in KiB; 0 when blocked
    size_kib: int
    mount: Optional[str] = None
    status: str = "ok"       # "ok" | "blocked"
    reason: Optional[str] = None

    def to_manifest_entry(self) -> dict[str, Any]:
        if self.status == "blocked":
            return {
                "name":         self.name,
                "fs":           self.fs,
                "flash_device": self.flash_device,
                "status":       "blocked",
                "reason":       self.reason or "",
            }
        entry: dict[str, Any] = {
            "name":          self.name,
            "fs":            self.fs,
            "flash_device":  self.flash_device,
            "dt_label":      self.dt_label,
            "offset_kib":    self.base_kib,
            "size_kib":      self.size_kib,
        }
        if self.mount:
            entry["mount"] = self.mount
        return entry


@dataclass
class BoardProject:
    """Resolved board.yaml project ready for fan-out."""

    sku: str
    hw_rev: Optional[str]
    board_name: Optional[str]
    board_hw_rev: Optional[str]
    cores: dict[str, Slice]                       # effective per-core slices
    ipc: list[IpcEntry]
    soc_spec: dict[str, Any]
    som_preset: dict[str, Any]
    board_preset: Optional[dict[str, Any]]
    diagnostics: dict[str, Any] = field(default_factory=dict)
    chips: list[str] = field(default_factory=list)
    features: dict[str, Any] = field(default_factory=dict)
    boot: dict[str, Any] = field(default_factory=dict)
    ota: dict[str, Any] = field(default_factory=dict)
    storage: list[StorageEntry] = field(default_factory=list)
    security: dict[str, Any] = field(default_factory=dict)
    raw: dict[str, Any] = field(default_factory=dict)


@dataclass
class SystemManifest:
    """The artefact written to build/system-manifest.yaml."""

    project: BoardProject
    slices: list[Slice]
    carve_outs: list[ResolvedCarveOut]
    partitions: list[ResolvedPartition] = field(default_factory=list)
    boot_order: list[dict[str, Any]] = field(default_factory=list)
    helper_mcus: list[dict[str, Any]] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "schema_version": 1,
            "generated_by":   "scripts/alp_orchestrate.py",
            "hw_info": {
                "sku":             self.project.sku,
                "som_hw_rev":      self.project.hw_rev,
                "board_name":    self.project.board_name,
                "board_hw_rev":  self.project.board_hw_rev,
                "silicon":         self.project.som_preset.get("silicon"),
            },
            "slices":      [s.to_manifest_entry() for s in self.slices],
            "ipc":         [c.to_manifest_entry() for c in self.carve_outs],
            "helper_mcus": list(self.helper_mcus),
            "boot_order":  list(self.boot_order),
        }
        if self.partitions:
            out["storage"] = [p.to_manifest_entry() for p in self.partitions]
        return out


# ---------------------------------------------------------------------
# Silicon ref -> SoC JSON path
# ---------------------------------------------------------------------

# Mirrors scripts/alp_project.py:_SILICON_TO_KCONFIG so the orchestrator
# can resolve any silicon ref it sees -- used to check the loader's
# Kconfig coverage matches the SoC spec coverage.
_SILICON_TO_KCONFIG: dict[str, str] = {
    "alif:ensemble:e3": "ALP_SOC_ALIF_ENSEMBLE_E3",
    "alif:ensemble:e4": "ALP_SOC_ALIF_ENSEMBLE_E4",
    "alif:ensemble:e5": "ALP_SOC_ALIF_ENSEMBLE_E5",
    "alif:ensemble:e6": "ALP_SOC_ALIF_ENSEMBLE_E6",
    "alif:ensemble:e7": "ALP_SOC_ALIF_ENSEMBLE_E7",
    "alif:ensemble:e8": "ALP_SOC_ALIF_ENSEMBLE_E8",
    "renesas:rzv2n:n44": "ALP_SOC_RENESAS_RZV2N_N44",
    "nxp:imx9:imx93":   "ALP_SOC_NXP_IMX9_IMX93",
}


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


# Curated `libraries:` enum, mirrored from
# metadata/schemas/board.schema.json `cores.<id>.libraries.items.enum`.
# Used by _validate_consistency() to reject `extra_libraries:` slugs
# that collide with the curated set (the curated library MUST be
# declared via `libraries:` -- the escape hatch is for non-curated
# entries only).
_CURATED_LIBRARIES: frozenset[str] = frozenset({
    "etl", "fmt", "nlohmann_json", "doctest", "lvgl",
    "mbedtls", "cmsis_dsp", "littlefs", "tflite_micro",
    "u8g2", "gfx_compat", "madgwick_ahrs", "pid", "modbus",
    "coremqtt_sn", "libcoap", "tinygsm", "nanopb",
    "libwebsockets", "jsmn", "bearssl", "minimp3", "opus",
    "libhelix", "catch2",
})


def _boot_signing_supported_for_family(
    family: str,
) -> Optional[frozenset[str]]:
    """Return the allowed `boot.signing.algorithm:` values for a SoM
    family, or None when the family is unknown (caller accepts any
    schema-enum value).

    Per the v0.6 P2.3 cross-field-validator pass:
      - Alif Ensemble (with OPTIGA Trust M as attestation root):
        `ecdsa_p256`, `ed25519` only.  RSA is excluded -- the OPTIGA
        slot type paired with the AEN secure-boot flow is ECC-only.
      - Renesas RZ/V2N (+ V2N + DEEPX): `ecdsa_p256`, `rsa2048`,
        `rsa3072`.  ed25519 is excluded (not in the U-Boot
        fit-signature stack used on V2N today).
      - NXP i.MX 9x: `ecdsa_p256`, `rsa2048`, `rsa3072`.
      - Unknown families: None (validator falls through to the schema
        enum -- don't block on missing capability data for
        in-development presets).
    """
    f = (family or "").lower()
    if f.startswith("alif-ensemble"):
        return frozenset({"ecdsa_p256", "ed25519"})
    if f.startswith("renesas-rzv2n"):
        return frozenset({"ecdsa_p256", "rsa2048", "rsa3072"})
    if f.startswith("nxp-imx9"):
        return frozenset({"ecdsa_p256", "rsa2048", "rsa3072"})
    return None


# TLS providers acceptable for `cores.<id>.iot.tls: true` (rule 3).
# A library named `mbedtls` or `bearssl` in either `libraries:` or
# `extra_libraries:` satisfies the requirement.
_TLS_PROVIDERS: frozenset[str] = frozenset({"mbedtls", "bearssl"})


def _validate_consistency(project: "BoardProject") -> None:
    """Cross-field validation that the schema can't express cleanly.

    Runs after the project is fully assembled by `load_board_yaml()`.
    Raises `OrchestratorError` for hard violations; emits stderr
    `WARN: ...` lines for soft signals (rules 4 + 5).

    Rules (v0.6 P2.3):

    1. `ota.provider: mender` requires at least one
       `cores.<id>.os: yocto` slice.  Reason: server-mode Mender is a
       Yocto-only flow today; the Zephyr-side dispatch
       (Mender-MCU-client) lands separately per ADR 0009.
    2. `boot.signing.algorithm:` must be in the SoM family's supported
       set (see `_boot_signing_supported_for_family`).  Unknown
       families pass through (don't block on missing capability data).
    3. `cores.<id>.iot.tls: true` requires `libraries:` (curated) OR
       `extra_libraries:` (open-set) to include `mbedtls` or `bearssl`.
    4. WARNING (not error): `cores.<id>.inference.default_arena_kib`
       larger than `cores.<id>.memory.heap_kib`; inference may OOM.
    5. WARNING (not error): `cores.<id>.power.sleep_mode != disabled`
       with no `wakeup_sources:` declared; device will sleep but can't
       wake.

    Also enforces the `extra_libraries:` invariants the schema can't
    cleanly express (P2.1):

      - Exactly one of `kconfig:` / `profile:` per entry.
      - Names globally unique across every core's `extra_libraries:`.
      - Names don't collide with the curated `libraries:` enum.
      - `profile:` paths resolve to a real file (repo-relative).
    """
    # ---- extra_libraries invariants (P2.1) -----------------------
    seen_names: dict[str, str] = {}    # name -> first core_id seen on
    for core_id, slice_ in project.cores.items():
        for idx, entry in enumerate(slice_.extra_libraries):
            name = entry.get("name")
            if not isinstance(name, str) or not name:
                raise OrchestratorError(
                    f"core '{core_id}' extra_libraries[{idx}]: missing "
                    f"or non-string `name:`")
            has_kc = "kconfig" in entry and entry.get("kconfig") is not None
            has_pf = "profile" in entry and entry.get("profile") is not None
            if has_kc == has_pf:
                # Both set OR both unset -- reject.
                raise OrchestratorError(
                    f"core '{core_id}' extra_libraries entry '{name}' "
                    f"must declare exactly one of `kconfig:` or "
                    f"`profile:` (got "
                    f"{'both' if has_kc and has_pf else 'neither'}).  "
                    f"Inline `kconfig:` is the fast path; `profile:` "
                    f"points at a hw-backends.yaml-style file.")
            if name in _CURATED_LIBRARIES:
                raise OrchestratorError(
                    f"core '{core_id}' extra_libraries entry '{name}' "
                    f"collides with the curated `libraries:` enum; "
                    f"declare curated libraries via "
                    f"`cores.{core_id}.libraries:` instead.")
            if name in seen_names:
                raise OrchestratorError(
                    f"core '{core_id}' extra_libraries entry '{name}' "
                    f"collides with the same name on core "
                    f"'{seen_names[name]}'; extra_libraries names "
                    f"must be globally unique across all cores.")
            seen_names[name] = core_id
            if has_pf:
                prof = entry.get("profile")
                if not isinstance(prof, str) or not prof:
                    raise OrchestratorError(
                        f"core '{core_id}' extra_libraries entry "
                        f"'{name}' has non-string `profile:`")
                prof_path = (REPO / prof).resolve()
                if not prof_path.is_file():
                    raise OrchestratorError(
                        f"core '{core_id}' extra_libraries entry "
                        f"'{name}' `profile: {prof}` does not resolve "
                        f"to a file at {prof_path}")

    # ---- Rule 1: ota.provider requires a compatible slice -------
    # Provider-driven dispatch (ADR 0009 follow-up resolved):
    #   mender   -> Yocto (server-mode) OR Zephyr (Mender-MCU-client)
    #   hawkbit  -> Zephyr (upstream Hawkbit DDI client)
    #   mcumgr   -> Zephyr (upstream MCUmgr / SMP)
    ota = project.ota or {}
    provider = (ota.get("provider") or "").lower() if isinstance(ota, dict) else ""
    if provider == "mender":
        has_target = any(s.os in ("yocto", "zephyr")
                         for s in project.cores.values())
        if not has_target:
            raise OrchestratorError(
                "ota.provider: mender requires at least one "
                "cores.<id>.os: yocto (server-mode Mender) OR "
                "cores.<id>.os: zephyr (Mender-MCU-client); none "
                "found.  See ADR 0009 for the provider-driven "
                "dispatch.")
    elif provider == "hawkbit":
        if not any(s.os == "zephyr" for s in project.cores.values()):
            raise OrchestratorError(
                "ota.provider: hawkbit requires at least one "
                "cores.<id>.os: zephyr (Hawkbit DDI client is in "
                "Zephyr upstream); none found.")
    elif provider == "mcumgr":
        if not any(s.os == "zephyr" for s in project.cores.values()):
            raise OrchestratorError(
                "ota.provider: mcumgr requires at least one "
                "cores.<id>.os: zephyr (MCUmgr is Zephyr's SMP "
                "transport for OTA); none found.")

    # ---- Rule 2: boot.signing.algorithm supported by SoM family --
    boot = project.boot or {}
    sign = (boot.get("signing") or {}) if isinstance(boot, dict) else {}
    algo = ((sign.get("algorithm") or "").lower()
            if isinstance(sign, dict) else "")
    if algo:
        family = (project.som_preset.get("family") or "")
        supported = _boot_signing_supported_for_family(family)
        if supported is not None and algo not in supported:
            raise OrchestratorError(
                f"boot.signing.algorithm: {algo} is not supported by "
                f"SoM family '{family}'; supported algorithms for "
                f"this family: {sorted(supported)}.  Edit "
                f"boot.signing.algorithm or pick a SoM family that "
                f"supports {algo}.")

    # ---- Rule 3: iot.tls: true requires mbedtls/bearssl ----------
    for core_id, slice_ in project.cores.items():
        if not (slice_.iot or {}).get("tls"):
            continue
        libs = set(slice_.libraries or [])
        extras = {e.get("name") for e in slice_.extra_libraries
                  if isinstance(e.get("name"), str)}
        if not (libs & _TLS_PROVIDERS) and not (extras & _TLS_PROVIDERS):
            raise OrchestratorError(
                f"core '{core_id}' has iot.tls: true but neither "
                f"`libraries:` nor `extra_libraries:` declares a TLS "
                f"provider (mbedtls or bearssl).  Add one of these "
                f"to cores.{core_id}.libraries: (curated) or "
                f"cores.{core_id}.extra_libraries: (open-set).")

    # ---- Rule 4: inference arena > heap is a WARN ----------------
    for core_id, slice_ in project.cores.items():
        arena_kib = (slice_.inference or {}).get("default_arena_kib")
        heap_kib = (slice_.memory or {}).get("heap_kib")
        if (isinstance(arena_kib, int) and isinstance(heap_kib, int)
                and arena_kib > heap_kib):
            print(
                f"WARN: cores.{core_id}.inference.default_arena_kib="
                f"{arena_kib} > cores.{core_id}.memory.heap_kib="
                f"{heap_kib}; inference may OOM",
                file=sys.stderr,
            )

    # ---- Rule 5: sleep_mode != disabled with no wakeup_sources --
    for core_id, slice_ in project.cores.items():
        pwr = slice_.power or {}
        sleep_mode = (pwr.get("sleep_mode") or "disabled").lower()
        wake = pwr.get("wakeup_sources") or []
        if sleep_mode != "disabled" and not wake:
            print(
                f"WARN: cores.{core_id}.power.sleep_mode={sleep_mode} "
                f"but no wakeup_sources: declared; device will sleep "
                f"but cannot wake",
                file=sys.stderr,
            )


def _enforce_loader_rules(slice_: Slice) -> None:
    """Loader rules from spec §4.5: every non-off slice must declare
    enough to actually build."""
    if slice_.os == "off":
        return
    if slice_.os == "zephyr":
        if not slice_.app:
            raise OrchestratorError(
                f"core '{slice_.core_id}': os: zephyr requires `app:` "
                f"pointing at a prj.conf / CMakeLists.txt directory")
    elif slice_.os == "baremetal":
        if not slice_.app:
            raise OrchestratorError(
                f"core '{slice_.core_id}': os: baremetal requires `app:` "
                f"pointing at a CMakeLists.txt directory")
    elif slice_.os == "yocto":
        if not slice_.app and not slice_.image:
            raise OrchestratorError(
                f"core '{slice_.core_id}': os: yocto requires either "
                f"`app:` (custom recipe) or `image:` (stock recipe)")
    elif slice_.os not in ("zephyr", "yocto", "baremetal", "off"):
        raise OrchestratorError(
            f"core '{slice_.core_id}': unknown os '{slice_.os}'")


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


_PAGE = 4096


def _fnv1a_32(data: bytes) -> int:
    """FNV-1a 32-bit hash.  10 lines, no deps."""
    h = 0x811c9dc5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def _region_size_bytes(region: dict[str, Any]) -> Optional[int]:
    """Convert a memory_map entry's size_mib / size_kib to bytes.

    Returns None if the size field is unset OR is the literal 'TBD'.
    """
    if "size_mib" in region:
        v = region["size_mib"]
        if isinstance(v, int):
            return v * 1024 * 1024
        return None
    if "size_kib" in region:
        v = region["size_kib"]
        if isinstance(v, int):
            return v * 1024
        return None
    return None


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
                f"`nxp_mu`, `alif_evtrtr`) or remove the rpmsg "
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
        base = region["base"]
        size_bytes = _region_size_bytes(region)
        if isinstance(base, str) and base.strip().upper() == "TBD":
            return None, (
                f"memory_map.base is TBD for region '{name}' in SoM "
                f"{project.sku}; this SoM hasn't been HW-mapped yet so "
                f"IPC carve-outs cannot be allocated.  Fill the value "
                f"in metadata/e1m_modules/{project.sku}.yaml or remove "
                f"the matching ipc entry from board.yaml.")
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


# ---------------------------------------------------------------------
# Storage partition resolver
# ---------------------------------------------------------------------


def _known_flash_devices(
    som_preset: dict[str, Any],
    metadata_root: Path,
) -> list[str]:
    """Enumerate every flash-device name a `storage[].flash_device:` may
    reference for the given SoM preset.

    Today this is the union of:
      - `memory_map:` region names (explicit override or derived from
        the SoC variant), AND
      - `on_module.ospi_memories:` keys (when declared on the SoM).

    Kept as a list so the loader's "did you mean..." message can sort
    it deterministically.
    """
    names: set[str] = set()
    for region in resolve_memory_map(som_preset, metadata_root):
        n = region.get("name")
        if isinstance(n, str):
            names.add(n)
    om = som_preset.get("on_module") or {}
    ospi = om.get("ospi_memories") or {}
    if isinstance(ospi, dict):
        for k in ospi.keys():
            if isinstance(k, str):
                names.add(k)
    return sorted(names)


def _resolve_flash_device(
    flash_device: str,
    som_preset: dict[str, Any],
    metadata_root: Path,
) -> tuple[Optional[dict[str, Any]], Optional[str]]:
    """Resolve a board.yaml `flash_device:` reference to a device descriptor.

    Returns `(descriptor, None)` on success; `(None, reason)` when the
    device is known but cannot be allocated against (TBD capacity).
    The descriptor carries:
        name        -- the SDK device name (verbatim from board.yaml)
        dt_label    -- the Zephyr DT label to decorate
        size_bytes  -- capacity in bytes (None when unresolvable)

    Address allocation is offset-within-device; the descriptor does
    NOT carry a physical base because Zephyr's flash-mapping layer
    derives that from the DT controller node.  The emitted overlay
    references `&<dt_label>` and lets Zephyr handle the physical mapping.
    """
    # memory_map: region match first (covers the auto-derived MRAM /
    # SRAM region table from resolve_memory_map()).
    for region in resolve_memory_map(som_preset, metadata_root):
        if region.get("name") != flash_device:
            continue
        size_bytes = _region_size_bytes(region)
        if size_bytes is None:
            return None, (
                f"flash device '{flash_device}' resolves to memory_map "
                f"region but size_mib / size_kib is unset or TBD on "
                f"SoM {som_preset.get('sku', '<unknown>')}; the SoM "
                f"hasn't been HW-mapped yet")
        dt_label = (region.get("dt_label")
                    if isinstance(region.get("dt_label"), str)
                    else flash_device)
        return {
            "name":       flash_device,
            "dt_label":   dt_label,
            "size_bytes": size_bytes,
        }, None

    # on_module.ospi_memories: key match.
    om = som_preset.get("on_module") or {}
    ospi = om.get("ospi_memories") or {}
    if isinstance(ospi, dict) and flash_device in ospi:
        entry = ospi[flash_device] or {}
        cap = entry.get("capacity_mbit")
        if isinstance(cap, str) and cap.strip().upper() == "TBD":
            return None, (
                f"on_module.ospi_memories.{flash_device}.capacity_mbit "
                f"is TBD on SoM {som_preset.get('sku', '<unknown>')}; "
                f"fill the value or move the storage entry to a sized "
                f"flash device")
        if not isinstance(cap, int):
            return None, (
                f"on_module.ospi_memories.{flash_device}.capacity_mbit "
                f"is missing on SoM {som_preset.get('sku', '<unknown>')}; "
                f"the storage allocator needs an authoritative capacity")
        dt_label = (entry.get("dt_label")
                    if isinstance(entry.get("dt_label"), str)
                    else flash_device)
        # capacity_mbit is megabits; convert to bytes.
        size_bytes = int(cap) * 1024 * 1024 // 8
        return {
            "name":       flash_device,
            "dt_label":   dt_label,
            "size_bytes": size_bytes,
        }, None

    # Unknown device — the loader catches this earlier, but defend
    # in depth in case a resolver is called with a hand-built project.
    return None, (
        f"flash device '{flash_device}' is neither a memory_map region "
        f"nor an on_module.ospi_memories key on SoM "
        f"{som_preset.get('sku', '<unknown>')}")


def _blocked_partition(
    entry: StorageEntry,
    reason: str,
) -> ResolvedPartition:
    """Project a StorageEntry into a blocked ResolvedPartition."""
    return ResolvedPartition(
        name=entry.name,
        fs=entry.fs,
        flash_device=entry.flash_device or "",
        dt_label="",
        base_kib=0,
        size_kib=entry.size_kib,
        mount=entry.mount,
        status="blocked",
        reason=reason,
    )


def resolve_storage_partitions(
    project: BoardProject,
) -> list[ResolvedPartition]:
    """Allocate physical offsets for every storage[] entry.

    Algorithm (mirrors `resolve_carve_outs()`):
      1. Group entries by `flash_device:` (entries without one block
         immediately; the loader normally catches that, but this is
         the resolver's failure mode for hand-built projects).
      2. Within each group, sort by name for determinism (an explicit
         `offset_kib:` doesn't change sort order — it's an override
         the allocator simply respects).
      3. For each entry: if `offset_kib:` is set, honour it (page-
         aligned check; overlap check against prior entries).  Else
         allocate bottom-up from the current high-water mark, page-
         aligned.
      4. Block on capacity overflow, TBD device, page-misaligned
         offset, or sibling overlap.  Blocked entries land in the
         manifest with `status: blocked` + `reason:` so reviewers see
         the gap; the slice build trips when the DTS overlay is
         consumed.

    Spec note: byte-stable OTA images require the orchestrator to pin
    addresses (resolved in design Q1 for v0.6 -- option "Pin in
    orchestrator").  Customers get reproducible per-rebuild addresses
    by declaring stable `name:` slugs; partitions stay at the same
    offset across rebuilds as long as their relative order doesn't
    change.  Explicit `offset_kib:` is the escape hatch.
    """
    if not project.storage:
        return []

    # Group by flash device, preserving original order for the
    # downstream resolver (we re-sort for allocation determinism below).
    by_device: dict[str, list[StorageEntry]] = {}
    no_device: list[StorageEntry] = []
    for entry in project.storage:
        if not entry.flash_device:
            no_device.append(entry)
        else:
            by_device.setdefault(entry.flash_device, []).append(entry)

    resolved: list[ResolvedPartition] = []

    for entry in no_device:
        resolved.append(_blocked_partition(entry, (
            f"storage entry '{entry.name}' has no flash_device: "
            f"declared; add one referencing a SoM memory_map region "
            f"or an on_module.ospi_memories key")))

    # Iterate flash devices in alphabetical order; within each device,
    # entries are name-sorted for byte-stable allocation.
    for device_name in sorted(by_device.keys()):
        descriptor, block_reason = _resolve_flash_device(
            device_name, project.som_preset, METADATA_ROOT)
        if descriptor is None:
            for entry in by_device[device_name]:
                resolved.append(_blocked_partition(
                    entry, block_reason or "flash device unresolvable"))
            continue

        dt_label = descriptor["dt_label"]
        capacity_bytes = descriptor["size_bytes"]
        # Page-aligned high-water mark; sibling partitions allocate
        # bottom-up from offset 0.  Page = 4 KiB matches the IPC
        # carve-out allocator; storage erase blocks on every silicon
        # the SDK targets are 4 KiB-or-larger multiples.
        high_water_bytes = 0
        # Track allocated [lo, hi) ranges so explicit `offset_kib:`
        # overrides can be checked against sibling allocations even
        # when the bump allocator would have placed them differently.
        allocated: list[tuple[int, int, str]] = []   # (lo, hi, name)

        entries_sorted = sorted(
            by_device[device_name], key=lambda e: e.name)

        for entry in entries_sorted:
            size_bytes = entry.size_kib * 1024
            size_aligned = ((size_bytes + _PAGE - 1) // _PAGE) * _PAGE

            if entry.offset_kib is not None:
                base_bytes = entry.offset_kib * 1024
                if base_bytes % _PAGE != 0:
                    resolved.append(_blocked_partition(entry, (
                        f"storage entry '{entry.name}' explicit "
                        f"offset_kib={entry.offset_kib} is not page-"
                        f"aligned (4 KiB)")))
                    continue
            else:
                base_bytes = high_water_bytes

            top_bytes = base_bytes + size_aligned
            if top_bytes > capacity_bytes:
                resolved.append(_blocked_partition(entry, (
                    f"storage entry '{entry.name}' ({entry.size_kib} "
                    f"KiB at offset {base_bytes // 1024} KiB) overruns "
                    f"flash device '{device_name}' "
                    f"(capacity {capacity_bytes // 1024} KiB)")))
                continue

            # Sibling-overlap check (only meaningful when offset_kib was
            # supplied; the bump allocator can't overlap with itself).
            overlap_with: Optional[str] = None
            for lo, hi, peer_name in allocated:
                if not (top_bytes <= lo or base_bytes >= hi):
                    overlap_with = peer_name
                    break
            if overlap_with is not None:
                resolved.append(_blocked_partition(entry, (
                    f"storage entry '{entry.name}' explicit "
                    f"offset_kib={entry.offset_kib} overlaps sibling "
                    f"partition '{overlap_with}' on device "
                    f"'{device_name}'")))
                continue

            allocated.append((base_bytes, top_bytes, entry.name))
            if entry.offset_kib is None:
                # Only bump the allocator when we used it; explicit
                # offsets don't shift the high-water mark (they may be
                # below it deliberately, e.g. to reserve a low slot).
                high_water_bytes = top_bytes

            resolved.append(ResolvedPartition(
                name=entry.name,
                fs=entry.fs,
                flash_device=device_name,
                dt_label=dt_label,
                base_kib=base_bytes // 1024,
                size_kib=size_aligned // 1024,
                mount=entry.mount,
            ))

    return resolved


# ---------------------------------------------------------------------
# Emitters
# ---------------------------------------------------------------------


def emit_ipc_contract_h(project: BoardProject) -> str:
    """Generate alp_system_ipc.h per spec §6.3."""
    carve_outs = resolve_carve_outs(project)

    lines: list[str] = [
        "/*",
        " * Auto-generated by scripts/alp_orchestrate.py -- do not edit.",
        " * Regenerate after changes to board.yaml `ipc:` or the SoM's",
        " * memory_map / mailbox blocks.",
        " */",
        "",
        "#ifndef ALP_SYSTEM_IPC_H",
        "#define ALP_SYSTEM_IPC_H",
        "",
        f'#define ALP_IPC_SKU "{project.sku}"',
        "",
    ]
    if not carve_outs:
        lines += [
            "/* No ipc[] entries declared in board.yaml; nothing to emit. */",
            "",
            "#endif /* ALP_SYSTEM_IPC_H */",
            "",
        ]
        return "\n".join(lines)

    for c in carve_outs:
        upper = c.name.upper()
        lines.append(f"/* {c.kind} channel '{c.name}' -- endpoints "
                     f"{', '.join(c.endpoints)} */")
        if c.status == "blocked":
            # Blocked entries still need the SRC_EPT / DST_EPT / MBOX_CH /
            # ADDR / SIZE macros so consumer source compiles: the
            # canonical pattern (see examples/rpmsg-*/m*/src/main.c) is a
            # static struct initialiser that references every field at
            # compile time.  Omitting the macros made every rpmsg
            # consumer fail with `<macro> undeclared` long before the
            # NOSUPPORT runtime check could even fire.  Emit safe-zero
            # stubs + a #warning so the build is green and the gap is
            # surfaced via the alp_rpc_open() ALP_ERR_NOSUPPORT path
            # the backend already returns when it sees a zero
            # mailbox_channel or empty mailbox controller.
            lines.append(f"/* BLOCKED: {c.reason or 'unknown reason'} */")
            # Surface the gap as a structured comment block rather than
            # a `#warning` (the slice builds under -Werror=cpp and an
            # actual warning would trip it).  The same reason text is
            # also written into the build manifest by the orchestrator's
            # fan-out step so reviewers see it.
            lines.append(f"/* IPC channel '{c.name}' is blocked; "
                         "fix the SoM metadata before depending on this "
                         "channel at runtime. */")
            lines.append(f'#define ALP_IPC_{upper}_NAME       "{c.name}"')
            lines.append(f"#define ALP_IPC_{upper}_ADDR       0x0u  /* stub: blocked */")
            lines.append(f"#define ALP_IPC_{upper}_SIZE       0x0u  /* stub: blocked */")
            lines.append(f"#define ALP_IPC_{upper}_SRC_EPT    0x0u  /* stub: blocked */")
            lines.append(f"#define ALP_IPC_{upper}_DST_EPT    0x0u  /* stub: blocked */")
            lines.append(f"#define ALP_IPC_{upper}_MBOX_CH    0u    /* stub: blocked */")
            lines.append("")
            continue
        lines.append(f'#define ALP_IPC_{upper}_NAME       "{c.name}"')
        lines.append(f'#define ALP_IPC_{upper}_ADDR       0x{c.base:08x}u')
        lines.append(f'#define ALP_IPC_{upper}_SIZE       0x{c.size:08x}u')
        lines.append(f'#define ALP_IPC_{upper}_SRC_EPT    0x{c.src_ept:08x}u')
        lines.append(f'#define ALP_IPC_{upper}_DST_EPT    0x{c.dst_ept:08x}u')
        lines.append(f'#define ALP_IPC_{upper}_MBOX_CH    {c.mailbox_channel}u')
        lines.append("")

    lines.append("#endif /* ALP_SYSTEM_IPC_H */")
    lines.append("")
    return "\n".join(lines)


def emit_dts_reservations(project: BoardProject) -> str:
    """Generate dts-reservations.dtsi per spec §6.2."""
    carve_outs = resolve_carve_outs(project)
    lines: list[str] = [
        "/*",
        " * Auto-generated by scripts/alp_orchestrate.py -- do not edit.",
        " * Regenerate after changes to board.yaml `ipc:` or the SoM's",
        " * memory_map block.  #include this file from your kernel /",
        " * Zephyr DT.",
        " */",
        "",
        "/ {",
        "    reserved-memory {",
        "        #address-cells = <2>;",
        "        #size-cells = <2>;",
        "",
    ]
    if not carve_outs:
        lines.append("        /* No ipc[] carve-outs declared. */")
    else:
        for c in carve_outs:
            if c.status == "blocked":
                # Blocked carve-outs land as a comment so reviewers see
                # the gap; the slice build trips elsewhere (the C
                # header carries an #error directive).
                lines.append(f"        /* BLOCKED: {c.name} -- "
                             f"{c.reason or 'unknown reason'} */")
                lines.append("")
                continue
            # Split base / size into two 32-bit halves for the
            # #address-cells = <2>; #size-cells = <2>; addressing.
            base_hi = (c.base >> 32) & 0xFFFFFFFF
            base_lo = c.base & 0xFFFFFFFF
            size_hi = (c.size >> 32) & 0xFFFFFFFF
            size_lo = c.size & 0xFFFFFFFF
            lines.append(
                f"        {c.name}: {c.name}@{c.base:x} {{")
            lines.append(
                '            compatible = "shared-dma-pool";')
            lines.append(
                f"            reg = <0x{base_hi:x} 0x{base_lo:08x} "
                f"0x{size_hi:x} 0x{size_lo:08x}>;")
            lines.append("            no-map;")
            lines.append(f'            label = "{c.name}";')
            lines.append("        };")
            lines.append("")

    lines.append("    };")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def emit_dts_partitions(project: BoardProject) -> str:
    """Generate dts-partitions.dtsi from board.yaml `storage:`.

    The overlay decorates each referenced flash device with a
    `partitions { ... }` child node carrying every resolved
    partition for that device.  Zephyr's `fixed-partitions` binding
    consumes the result; downstream Kconfig (CONFIG_FILE_SYSTEM_LITTLEFS
    etc., emitted by `_slice_alp_conf()`) wires the filesystem driver.

    Blocked partitions appear as bare comments so reviewers see the
    gap; the slice build trips elsewhere (the per-fs Kconfig will not
    have a backing partition and Zephyr's link-time fs-table assembly
    surfaces the error).
    """
    partitions = resolve_storage_partitions(project)
    lines: list[str] = [
        "/*",
        " * Auto-generated by scripts/alp_orchestrate.py -- do not edit.",
        " * Regenerate after changes to board.yaml `storage:` or the SoM's",
        " * memory_map / on_module.ospi_memories blocks.  #include this file",
        " * from your Zephyr DT.",
        " */",
        "",
    ]
    if not partitions:
        lines += [
            "/* No `storage:` entries declared in board.yaml; nothing to emit. */",
            "",
        ]
        return "\n".join(lines)

    # Group resolved partitions by dt_label; blocked entries get a
    # standalone comment block so reviewers see the gap.
    by_label: dict[str, list[ResolvedPartition]] = {}
    blocked: list[ResolvedPartition] = []
    for p in partitions:
        if p.status == "blocked":
            blocked.append(p)
            continue
        by_label.setdefault(p.dt_label, []).append(p)

    for label in sorted(by_label.keys()):
        lines.append(f"&{label} {{")
        lines.append("    partitions {")
        lines.append('        compatible = "fixed-partitions";')
        lines.append("        #address-cells = <1>;")
        lines.append("        #size-cells = <1>;")
        lines.append("")
        # Sort by base offset for human readability + DT-binding
        # convention (low-address first).
        for p in sorted(by_label[label], key=lambda x: x.base_kib):
            base_bytes = p.base_kib * 1024
            size_bytes = p.size_kib * 1024
            lines.append(
                f"        {p.name}_partition: partition@{base_bytes:x} {{")
            lines.append(f'            label = "{p.name}";')
            lines.append(
                f"            reg = <0x{base_bytes:x} 0x{size_bytes:x}>;")
            lines.append("        };")
            lines.append("")
        lines.append("    };")
        lines.append("};")
        lines.append("")

    if blocked:
        lines.append(
            "/* Blocked storage entries -- see system-manifest.yaml for details. */")
        for p in blocked:
            lines.append(f"/* BLOCKED: {p.name} -- "
                         f"{p.reason or 'unknown reason'} */")
        lines.append("")

    return "\n".join(lines)


def emit_storage_mounts_c(project: BoardProject) -> str:
    """Generate storage_mount_table.c -- a static fs_mount_t[] array.

    Optional companion to emit_dts_partitions.  Apps that want
    deterministic boot-time mounting include this file and call
    `alp_storage_mount_all()` (or iterate the table manually).
    Partitions with no `mount:` declared are omitted from the table.
    """
    partitions = resolve_storage_partitions(project)
    mountable = [p for p in partitions
                 if p.status == "ok" and p.mount and p.fs != "raw"]

    lines: list[str] = [
        "/*",
        " * Auto-generated by scripts/alp_orchestrate.py -- do not edit.",
        " * Regenerate after changes to board.yaml `storage:`.",
        " */",
        "/* clang-format off */",
        "",
        "#include <zephyr/fs/fs.h>",
        "#include <zephyr/storage/flash_map.h>",
        "",
    ]
    if not mountable:
        lines += [
            "/* No mountable storage[] entries declared; emitting empty table. */",
            "",
            "const struct fs_mount_t *alp_storage_mounts[] = { 0 };",
            "const size_t alp_storage_mount_count = 0;",
            "/* clang-format on */",
            "",
        ]
        return "\n".join(lines)

    # Per-fs includes + per-partition fs_<name>_data + fs_mount_t.
    fs_seen: set[str] = set()
    for p in mountable:
        if p.fs in fs_seen:
            continue
        fs_seen.add(p.fs)
        if p.fs == "littlefs":
            lines.append("#include <zephyr/fs/littlefs.h>")
        elif p.fs == "fat":
            lines.append("#include <ff.h>")
        elif p.fs == "ext4":
            lines.append("#include <zephyr/fs/ext2.h>")
    lines.append("")

    for p in mountable:
        upper = p.name.upper()
        if p.fs == "littlefs":
            lines.append(f"FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(alp_lfs_{p.name});")
            lines.append(f"static struct fs_mount_t alp_mnt_{p.name} = {{")
            lines.append(f"    .type = FS_LITTLEFS,")
            lines.append(f"    .fs_data = &alp_lfs_{p.name},")
            lines.append(f"    .storage_dev = (void *)FIXED_PARTITION_ID({p.name}_partition),")
            lines.append(f'    .mnt_point = "{p.mount}",')
            lines.append("};")
        elif p.fs == "fat":
            lines.append(f"static FATFS alp_fat_{p.name};")
            lines.append(f"static struct fs_mount_t alp_mnt_{p.name} = {{")
            lines.append(f"    .type = FS_FATFS,")
            lines.append(f"    .fs_data = &alp_fat_{p.name},")
            lines.append(f"    .storage_dev = (void *)FIXED_PARTITION_ID({p.name}_partition),")
            lines.append(f'    .mnt_point = "{p.mount}",')
            lines.append("};")
        elif p.fs == "ext4":
            lines.append(f"static struct fs_mount_t alp_mnt_{p.name} = {{")
            lines.append(f"    .type = FS_EXT2,")
            lines.append(f"    .storage_dev = (void *)FIXED_PARTITION_ID({p.name}_partition),")
            lines.append(f'    .mnt_point = "{p.mount}",')
            lines.append("};")
        lines.append("")
        # Silence -Wunused on the per-partition mount when the app
        # iterates only via alp_storage_mounts[]; the explicit symbol
        # lets test fixtures reach a specific mount by name.
        _ = upper

    lines.append("const struct fs_mount_t *alp_storage_mounts[] = {")
    for p in mountable:
        lines.append(f"    &alp_mnt_{p.name},")
    lines.append("};")
    lines.append("const size_t alp_storage_mount_count = "
                 f"{len(mountable)};")
    lines.append("/* clang-format on */")
    lines.append("")
    return "\n".join(lines)


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
        build/<core>-<os>/."""
        slice_dir = self.build_root / f"{slice_.core_id}-{slice_.os}"
        slice_dir.mkdir(parents=True, exist_ok=True)
        slice_.build_dir = slice_dir
        slice_.log_path = slice_dir / "build.log"
        if slice_.os == "zephyr":
            (slice_dir / "alp.conf").write_text(
                _slice_alp_conf(self.project, slice_), encoding="utf-8")
        elif slice_.os == "yocto":
            (slice_dir / "local.conf").write_text(
                _slice_local_conf(self.project, slice_), encoding="utf-8")
        elif slice_.os == "baremetal":
            (slice_dir / "cmake-args.txt").write_text(
                _slice_cmake_args(self.project, slice_), encoding="utf-8")
        return slice_dir

    def _materialise_shared(self) -> Path:
        gen = self.build_root / "generated"
        gen.mkdir(parents=True, exist_ok=True)
        # `<alp/system_ipc.h>` is the canonical include path consumers
        # use (see include/alp/rpc.h §usage and the per-slice main.c
        # references) — write the generated header at the matching
        # `alp/` subdir so slice CMakeLists can add this directory
        # straight to the include path.
        alp_subdir = gen / "alp"
        alp_subdir.mkdir(parents=True, exist_ok=True)
        (alp_subdir / "system_ipc.h").write_text(
            emit_ipc_contract_h(self.project), encoding="utf-8")
        (gen / "dts-reservations.dtsi").write_text(
            emit_dts_reservations(self.project), encoding="utf-8")
        # Storage partitions land alongside the IPC reservations DTS;
        # apps that don't declare storage[] still get a stub file with
        # a "nothing to emit" comment so downstream `#include` resolves.
        (gen / "dts-partitions.dtsi").write_text(
            emit_dts_partitions(self.project), encoding="utf-8")
        # TF-M sysbuild child-image overlay -- only when the project
        # opts in to `security.psa.tfm: true`.  Absence-emits-nothing:
        # we don't create the directory when the file would be empty.
        tfm_conf = emit_tfm_sysbuild_conf(self.project)
        if tfm_conf:
            tfm_dir = self.build_root / "sysbuild" / "tfm"
            tfm_dir.mkdir(parents=True, exist_ok=True)
            (tfm_dir / "tfm.conf").write_text(tfm_conf, encoding="utf-8")
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


# ---------------------------------------------------------------------
# Per-slice config emitters (consumed by both Orchestrator and the
# new --core <id> --emit zephyr-conf / yocto-conf modes in alp_project.py).
# ---------------------------------------------------------------------


# on_module fields that carry non-chip-slug values — skip them when
# walking the block for chip-driver enables.  Numeric fields, silicon
# identifiers, and structured sub-blocks are excluded by name rather
# than by type so the logic stays explicit and easy to audit.
_ON_MODULE_NON_CHIP_FIELDS: frozenset[str] = frozenset({
    "silicon",             # e.g. "renesas:rzv2n:n44" — SoC identifier, not a driver
    "ethernet_phy_count",  # integer count, not a chip slug
    "i2c_devices",         # sub-block: handled by extracting chip: entries below
    "ospi_memories",       # sub-block: handled by extracting chip: entries below
    # Storage-class fields encode the SoC controller / peripheral name
    # that reaches the on-module storage (e.g. `nor_flash: xspi` -> the
    # NOR flash is wired to the xSPI controller; `emmc: sd0` -> eMMC on
    # SD/MMC controller 0).  They are routing annotations, not chip
    # slugs, and have no `chips/<part>/` driver behind them; emitting
    # them as CHIP_<NAME> trips the Zephyr build with an undefined-symbol
    # warning (no CONFIG_ALP_SDK_CHIP_XSPI / SD0 declaration exists).
    "nor_flash",
    "emmc",
})


def _slugs_from_on_module(on_module: dict) -> list[str]:
    """Extract unique, non-TBD chip slugs from an ``on_module:`` block.

    Walks every scalar field that is NOT in ``_ON_MODULE_NON_CHIP_FIELDS``,
    then recurses into the ``ospi_memories`` sub-block (extracting the
    ``chip:`` field from each memory entry) and the ``i2c_devices``
    sub-block (extracting the ``chip:`` field from each device entry).
    Duplicate slugs and values of ``TBD`` / ``null`` are silently dropped.

    Returns a sorted, deduplicated list of slug strings.
    """
    seen: set[str] = set()

    def _add(val: object) -> None:
        if not val or val == "TBD":
            return
        if not isinstance(val, str):
            return
        seen.add(val)

    # 1. Scalar fields — every key whose value is a plain string and
    #    is not in the exclusion list.
    for key, val in on_module.items():
        if key in _ON_MODULE_NON_CHIP_FIELDS:
            continue
        if isinstance(val, str):
            _add(val)

    # 2. ospi_memories sub-block — each value is a dict with a `chip:`
    #    key.
    ospi = on_module.get("ospi_memories")
    if isinstance(ospi, dict):
        for _slot, entry in ospi.items():
            if isinstance(entry, dict):
                _add(entry.get("chip"))

    # 3. i2c_devices sub-block — each bus entry contains a `devices:`
    #    list; extract the `chip:` field from each device.
    #    Devices marked `assembled: optional` are DNI (do-not-install)
    #    on some builds and must NOT be auto-enabled as chip drivers —
    #    the customer explicitly enables them via `board.populated:`.
    i2c_buses = on_module.get("i2c_devices")
    if isinstance(i2c_buses, dict):
        for _bus, bus_entry in i2c_buses.items():
            if not isinstance(bus_entry, dict):
                continue
            for dev in bus_entry.get("devices") or []:
                if isinstance(dev, dict):
                    if dev.get("assembled") == "optional":
                        continue
                    _add(dev.get("chip"))

    return sorted(seen)


def _slugs_from_helper_firmware(helper_firmware: list) -> list[str]:
    """Extract unique, non-TBD chip slugs from a ``helper_firmware:`` list.

    Each entry is a dict; we pull the ``chip:`` field.  TBD values and
    missing fields are skipped.  Returns a sorted, deduplicated list.
    """
    seen: set[str] = set()
    for entry in helper_firmware or []:
        if not isinstance(entry, dict):
            continue
        chip = entry.get("chip")
        if chip and chip != "TBD":
            seen.add(chip)
    return sorted(seen)


def _emit_extra_library_profile(
    name: str,
    profile_rel: str,
    project: "BoardProject",
) -> list[str]:
    """Walk an extra_libraries `profile:` file and emit the per-class
    first-match Kconfig lines for the active SoM.

    Mirrors the curated `metadata/library-profiles/<lib>/hw-backends.yaml`
    matcher in `alp_project._emit_library_hw_backends`: each accelerator
    class declares a `priority:` list whose entries carry
    `silicon:` / `soc_family:` / `requires_cap:` matchers plus a
    `kconfig:` directive.  First matching entry per class wins; an
    `sw_fallback:` block with `kconfig:` is always emitted at the end
    so the build has a SW floor when no HW backend matches.

    Returns the list of emitted lines (each line is a complete
    `CONFIG_*=y` or `# ...` comment); empty list when the profile
    parses but nothing matches and there's no sw_fallback.

    Failures (malformed YAML, missing keys) emit a single `#`-prefixed
    diagnostic comment so the customer sees the failure in the slice's
    alp.conf rather than getting silent drop-out.
    """
    profile_path = (REPO / profile_rel).resolve()
    try:
        doc = yaml.safe_load(profile_path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as e:
        return [f"# extra_libraries[{name}] profile parse failed: {e}"]
    if not isinstance(doc, dict):
        return [f"# extra_libraries[{name}] profile is not a mapping"]

    # Match keys come from the active SoM.  Mirror
    # alp_project._SOC_FAMILY_TOKEN so extra_libraries entries can
    # share existing curated-profile match keys when desired.
    silicon_ref = project.som_preset.get("silicon") or ""
    family = (project.som_preset.get("family") or "").lower()
    soc_family_token: Optional[str] = None
    if family.startswith("alif-ensemble"):
        soc_family_token = "alif_ensemble"
    elif family.startswith("renesas-rzv2n"):
        soc_family_token = "renesas_rzv2n"
    elif family.startswith("nxp-imx9"):
        soc_family_token = "nxp_imx9"

    # resolve_capabilities merges SoC-JSON defaults + SoM overrides.
    capabilities = resolve_capabilities(project.som_preset, METADATA_ROOT)

    def _cap_truthy(cap_name: str) -> bool:
        v = capabilities.get(cap_name)
        if v is None:
            return False
        if isinstance(v, bool):
            return v
        if isinstance(v, int):
            return v > 0
        s = str(v).lower()
        if s in ("true", "yes"):
            return True
        if s in ("false", "no", "null", "none", "0", ""):
            return False
        try:
            return int(s) > 0
        except ValueError:
            return False

    def _entry_matches(e: dict[str, Any]) -> bool:
        sili = e.get("silicon")
        sf = e.get("soc_family")
        cap = e.get("requires_cap")
        if sili is not None and sili != silicon_ref:
            return False
        if sf is not None and sf != soc_family_token:
            return False
        if cap is not None and not _cap_truthy(str(cap)):
            return False
        return True

    out: list[str] = []
    accelerators = doc.get("accelerators") or []
    if isinstance(accelerators, list):
        for cls in accelerators:
            if not isinstance(cls, dict):
                continue
            cls_name = cls.get("class") or "<unknown>"
            for p in (cls.get("priority") or []):
                if not isinstance(p, dict):
                    continue
                if not _entry_matches(p):
                    continue
                kc = p.get("kconfig")
                if isinstance(kc, str) and kc:
                    out.append(f"{kc}  # {name} / {cls_name}")
                break

    sw = doc.get("sw_fallback")
    if isinstance(sw, dict):
        kc = sw.get("kconfig")
        if isinstance(kc, str) and kc:
            out.append(f"{kc}  # {name} / sw_fallback")

    return out


def _slice_alp_conf(project: BoardProject, slice_: Slice) -> str:
    """Per-core Kconfig fragment for a Zephyr slice.

    Emits the full Kconfig the slice needs: baseline + log + silicon +
    per-core peripherals/libraries + SoM-intrinsic chip drivers (auto-
    derived from ``on_module:`` + ``helper_firmware:`` in the SoM preset)
    + board-populated chip drivers (from board.yaml ``board.populated:``
    + the board preset) + the Zephyr subsystem enables those chip drivers
    need (e.g. an enabled ``rv3028c7`` chip driver pulls in ``CONFIG_I2C=y``).

    Swapping ``som.sku:`` in board.yaml automatically changes the SoM-
    intrinsic chip set with no other board.yaml edits required.
    """
    silicon = project.som_preset.get("silicon")
    kconfig = _SILICON_TO_KCONFIG.get(silicon)
    diagnostics = project.diagnostics

    # Lazy-import alp_project tables — alp_project imports us, so a
    # top-level import would cycle.  Only paid when emitting Zephyr
    # fragments.
    import sys as _sys
    from pathlib import Path as _Path
    _scripts = _Path(__file__).resolve().parent
    if str(_scripts) not in _sys.path:
        _sys.path.insert(0, str(_scripts))
    from alp_project import (  # type: ignore
        _CHIP_SUBSYSTEMS,
        _LIBRARY_KCONFIG,
    )

    lines: list[str] = []
    lines.append("# Auto-generated by scripts/alp_orchestrate.py "
                 "-- do not edit.")
    lines.append(f"# Per-core Kconfig fragment for slice "
                 f"`{slice_.core_id}` ({slice_.os}).")
    lines.append("")
    lines.append("CONFIG_ALP_SDK=y")
    lines.append("CONFIG_LOG=y")
    lines.append("CONFIG_PRINTK=y")
    if diagnostics.get("last_error", True):
        lines.append("CONFIG_THREAD_LOCAL_STORAGE=y")
    log_level = diagnostics.get("log_level")
    if log_level is not None:
        log_level_kc = {
            "error": 1, "warn": 2, "info": 3, "debug": 4, "trace": 4,
        }
        if log_level in log_level_kc:
            lines.append(f"CONFIG_LOG_DEFAULT_LEVEL={log_level_kc[log_level]}")
    lines.append("")
    if kconfig:
        lines.append(f"# SoM silicon ({silicon} via {project.sku})")
        lines.append(f"CONFIG_{kconfig}=y")
        lines.append("")

    # ----------------------------------------------------------------
    # SoM-intrinsic chip drivers — derived from on_module: + helper_firmware:
    # in the SoM preset.  These are NOT declared by the customer; they
    # are determined by which SoM SKU the project targets.  Swapping
    # `som.sku:` from E1M-V2N101 to E1M-AEN701 automatically swaps
    # the on-module chip set without any board.yaml changes.
    # ----------------------------------------------------------------
    som_chips: set[str] = set()
    om = project.som_preset.get("on_module") or {}
    if om:
        for slug in _slugs_from_on_module(om):
            som_chips.add(slug)
    hf = project.som_preset.get("helper_firmware") or []
    for slug in _slugs_from_helper_firmware(hf):
        som_chips.add(slug)

    # Slugs that map to BLOCK_ Kconfig symbols rather than CHIP_.
    # These live under blocks/ + <alp/blocks/*.h> because they are
    # SDK-level *block* utilities (`alp_button_led_*`, `alp_pdm_mic_*`)
    # rather than third-party-IC chip drivers; see blocks/README.md.
    _BLOCK_SLUGS = frozenset({"button_led", "pdm_mic"})

    def _slug_kconfig(slug: str) -> str:
        kind = "BLOCK" if slug in _BLOCK_SLUGS else "CHIP"
        return f"CONFIG_ALP_SDK_{kind}_{slug.upper()}"

    chip_subsystems: set[str] = set()
    if som_chips:
        sku_str = project.sku
        lines.append(f"# SoM-intrinsic chip drivers (from `{sku_str}` "
                     f"on_module + helper_firmware)")
        for chip in sorted(som_chips):
            lines.append(f"{_slug_kconfig(chip)}=y")
            for s in _CHIP_SUBSYSTEMS.get(chip, ()):
                chip_subsystems.add(s)
        lines.append("")

    # ----------------------------------------------------------------
    # Board-populated chip drivers.  Single source: the resolved
    # board_preset dict, which comes from either the shared
    # metadata/boards/<preset>.yaml or the project's inline
    # top-level fields (synthesised by the loader).  No project-level
    # override merge -- the schema's `oneOf` rule rejects mixing.
    #
    # Chip drivers compile per-chip via the
    # zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_<NAME> ...)
    # (or CONFIG_ALP_SDK_BLOCK_<NAME> for the `button_led` / `pdm_mic`
    # SDK-level block helpers) entries in zephyr/CMakeLists.txt.
    #
    # We emit the chip-driver block on every zephyr slice — Kconfig
    # dedupes when multiple per-core fragments overlay onto the same
    # base.  This keeps `--core <id>` invocations self-sufficient
    # (the slice carries everything it needs to compile) without
    # depending on cross-slice ordering.
    # ----------------------------------------------------------------
    populated: dict[str, bool] = dict(
        (project.board_preset or {}).get("populated") or {})
    if populated:
        lines.append("# Board-populated chip drivers (from the resolved "
                     "board definition)")
        for chip, on in sorted(populated.items()):
            # Deduplicate: if the SoM block already emitted =y, skip
            # the board line to avoid redundant CONFIG entries.
            if on and chip in som_chips:
                continue
            lines.append(f"{_slug_kconfig(chip)}={'y' if on else 'n'}")
            if on:
                for s in _CHIP_SUBSYSTEMS.get(chip, ()):
                    chip_subsystems.add(s)
        lines.append("")

    # Project-declared chips (board.yaml top-level `chips:` array).
    # Adds chip drivers the application links directly via
    # <alp/chips/<name>.h> -- e.g. when the project plugs an external
    # sensor into a board's headers that's not in the board's
    # `populated:` table.  Each entry maps to CHIP_<NAME>=y (or
    # BLOCK_<NAME>=y for the SDK-level block helpers).
    project_chips = [c for c in (project.chips or [])
                     if c not in som_chips and not populated.get(c)]
    if project_chips:
        lines.append("# Project-declared chips (board.yaml `chips:` array)")
        for chip in sorted(set(project_chips)):
            lines.append(f"{_slug_kconfig(chip)}=y")
            for s in _CHIP_SUBSYSTEMS.get(chip, ()):
                chip_subsystems.add(s)
        lines.append("")

    # Zephyr subsystems: union of (chip-driver-required subsystems for
    # the first zephyr core's chip block) + (this core's `peripherals:`
    # array, which adds to the union per spec §4.6).
    periph_subsystems: set[str] = set()
    for periph in slice_.peripherals or []:
        kc = _PERIPHERAL_KCONFIG.get(periph)
        if kc:
            periph_subsystems.add(kc)
    all_subsystems = chip_subsystems | periph_subsystems
    if all_subsystems:
        lines.append(f"# Zephyr subsystems required on core "
                     f"`{slice_.core_id}` (chip drivers + peripherals)")
        for s in sorted(all_subsystems):
            lines.append(f"CONFIG_{s}=y")
        lines.append("")

    if slice_.libraries:
        lines.append(f"# Libraries declared on core "
                     f"`{slice_.core_id}`")
        for lib in sorted(slice_.libraries):
            kcs = _LIBRARY_KCONFIG.get(lib)
            if kcs:
                for kc in kcs:
                    lines.append(kc)
            else:
                lines.append(
                    f"# TODO: wire library '{lib}' once its v0.4 enable lands")
        lines.append("")

    # ----------------------------------------------------------------
    # Open-set extra_libraries (v0.6 P2.1).  Each entry declares either
    # inline `kconfig:` lines (emitted verbatim) or a `profile:` path
    # to a hw-backends.yaml-style file we walk with the same silicon /
    # soc_family / requires_cap matcher as the curated libraries.
    # _validate_consistency() guarantees exactly-one of kconfig/profile
    # and the profile file resolves, so the per-entry checks here are
    # just for emit-time safety.
    # ----------------------------------------------------------------
    if slice_.extra_libraries:
        lines.append(f"# Extra libraries declared on core "
                     f"`{slice_.core_id}` (open-set escape hatch)")
        for entry in sorted(slice_.extra_libraries,
                            key=lambda e: e.get("name", "")):
            name = entry.get("name", "<unnamed>")
            kc_lines = entry.get("kconfig")
            profile = entry.get("profile")
            if kc_lines:
                lines.append(f"# extra_libraries[{name}] (inline kconfig)")
                for kc in kc_lines:
                    lines.append(str(kc))
            elif profile:
                lines.append(f"# extra_libraries[{name}] (profile: {profile})")
                profile_lines = _emit_extra_library_profile(
                    name, profile, project)
                if profile_lines:
                    lines.extend(profile_lines)
                else:
                    lines.append(
                        f"# (no matching backend for "
                        f"silicon={project.som_preset.get('silicon')} "
                        f"in {profile})")
        lines.append("")

    # Inference dispatchers.  Driven entirely by the SoM preset's
    # `capabilities:` matrix + SoC-JSON `cores[]` -- NOT by board.yaml.
    # The customer never picks a backend at build time; the SDK compiles
    # in every dispatcher the silicon supports, and apps choose
    # per-handle at runtime via alp_inference_open(.backend=...).
    # CPU/TFLM is the universal SW fallback and is always on.
    #
    # Two layers of variant detail are emitted alongside the umbrella
    # switches so the SDK driver code + (upstream) kernel libraries
    # compile against the right per-silicon variant:
    #
    #   - CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U{55,65,85}=y -- picked from
    #     the SoM preset's `npu_population:` list (preferred) with a
    #     capability-count fallback for SoMs that haven't declared the
    #     fine-grained population block yet.  U85 carries Arm's larger
    #     MAC array + TensorOptimized kernels; U55 carries the smaller
    #     MAC + reference kernels; U65 is i.MX 93-only.
    #
    #   - CONFIG_ALP_SDK_INFERENCE_TFLM_{NEON,HELIUM,REF}=y -- picked
    #     from the SoC JSON's `cores[<slice.core_id>].vector_extension`
    #     so the CPU-side TFLM kernel set matches the target core's SIMD
    #     reality (NEON on A-cluster, Helium MVE on M55, scalar / REF
    #     on baseline M33 / M4).  Per-core in case a single SoM hosts
    #     multiple core classes (E7 = A32 + M55, all three Helium /
    #     Neon flavours).
    #
    # resolve_capabilities merges SoC-JSON defaults with SoM overrides
    # so silicon-determined caps (ethos_u55_count, drp_ai, ...) resolve
    # even when removed from the SoM YAML (capability unification, slice 3b).
    capabilities = resolve_capabilities(project.som_preset, METADATA_ROOT)
    inference_lines: list[str] = ["CONFIG_ALP_SDK_INFERENCE_BACKEND_TFLM=y"]

    # ---- G-2 -- CPU-class TFLM kernel selector --------------------
    # Look up this slice's core in the SoC spec; pick exactly one of
    # NEON / HELIUM / REF based on the vector_extension field.  Defaults
    # to REF when the SoC JSON is silent (paper-correct on the scalar
    # M33s -- iMX 93 m33, V2N m33_sm).
    tflm_kernel_kc: str = "CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_REF=y"
    for c in (project.soc_spec.get("cores") or []):
        if c.get("id") != slice_.core_id:
            continue
        vec = (c.get("vector_extension") or "").lower()
        ctype = (c.get("type") or "").lower()
        if vec == "neon" or ctype.startswith("cortex-a"):
            tflm_kernel_kc = "CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_NEON=y"
        elif vec == "helium":
            tflm_kernel_kc = "CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_HELIUM=y"
        else:
            tflm_kernel_kc = "CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_REF=y"
        break
    inference_lines.append(tflm_kernel_kc)

    # ---- G-1 -- per-variant Ethos-U selector ---------------------
    # Prefer the SoM preset's `inference.npu_population[]` (richer --
    # also names the role + paired-core); fall back to the capability
    # counts (ethos_u{55,65,85}_count) for SoMs that haven't yet
    # declared the per-instance block.  Both AEN401 / AEN601 / AEN801
    # populate npu_population[]; AEN701 declares U55s there too; the
    # i.MX 93 SoM relies on the capability-count fallback today.
    ethos_variants: set[str] = set()
    npu_pop = (project.som_preset.get("inference") or {}).get("npu_population") or []
    for entry in npu_pop:
        v = (entry.get("variant") if isinstance(entry, dict) else "") or ""
        v = v.lower()
        if v in ("u55", "u65", "u85"):
            ethos_variants.add(v)
    # Capability-count fallback (handles SoMs without npu_population:).
    if (capabilities.get("ethos_u55_count") or 0) > 0:
        ethos_variants.add("u55")
    if (capabilities.get("ethos_u65_count") or 0) > 0:
        ethos_variants.add("u65")
    if (capabilities.get("ethos_u85_count") or 0) > 0:
        ethos_variants.add("u85")
    ethos_present = bool(ethos_variants)
    if ethos_present:
        # Per-silicon Ethos-U backend (Slice 3 registry layout):
        # Alif Ensemble (AEN) -> _BACKEND_ETHOS_U_AEN
        # NXP i.MX 93        -> _BACKEND_ETHOS_U_N93
        if silicon == "nxp:imx9:imx93":
            inference_lines.append("CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_N93=y")
        else:
            inference_lines.append("CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_AEN=y")
        for v in sorted(ethos_variants):
            inference_lines.append(f"CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_{v.upper()}=y")
    if capabilities.get("drp_ai"):
        inference_lines.append("CONFIG_ALP_SDK_INFERENCE_BACKEND_DRPAI_V2N=y")
    # DEEPX DX-M1 dispatches via Linux PCIe driver -- no Zephyr Kconfig.
    # Its build wiring lives on the cmake-args / Yocto emit paths.
    lines.append(f"# Inference dispatchers (from SoM capabilities -- "
                 f"customer does not pick)")
    lines.extend(inference_lines)
    lines.append("")

    # ----------------------------------------------------------------
    # Per-slice memory tuning (board.yaml `cores.<id>.memory:`).
    # ----------------------------------------------------------------
    mem = slice_.memory or {}
    mem_lines: list[str] = []
    if mem.get("stack_kib"):
        mem_lines.append(f"CONFIG_MAIN_STACK_SIZE={int(mem['stack_kib']) * 1024}")
    if mem.get("isr_stack_kib"):
        mem_lines.append(f"CONFIG_ISR_STACK_SIZE={int(mem['isr_stack_kib']) * 1024}")
    if mem.get("heap_kib") is not None:
        mem_lines.append(f"CONFIG_HEAP_MEM_POOL_SIZE={int(mem['heap_kib']) * 1024}")
    if mem_lines:
        lines.append("# Per-slice memory tuning (board.yaml cores.<id>.memory:)")
        lines.extend(mem_lines)
        lines.append("")

    # ----------------------------------------------------------------
    # Per-slice power-management profile.
    # ----------------------------------------------------------------
    pwr = slice_.power or {}
    pwr_lines: list[str] = []
    sleep_mode = (pwr.get("sleep_mode") or "disabled").lower()
    if sleep_mode != "disabled":
        pwr_lines.append("CONFIG_PM=y")
        pwr_lines.append("CONFIG_PM_DEVICE=y")
        # PM_STATE_* selection is silicon-family-specific; lowest-power
        # state the hierarchy keeps is mirrored in a single hint flag.
        pwr_lines.append(f"# Sleep target: {sleep_mode}")
    for wake in (pwr.get("wakeup_sources") or []):
        # Wake-source declarations land as hint comments only.  Zephyr
        # marks a device as wakeup-capable via the DT `wakeup-source;`
        # property + a runtime pm_device_wakeup_enable() call; there is
        # no top-level CONFIG_PM_DEVICE_WAKE_<SUBSYS> Kconfig symbol, so
        # emitting one trips the build with `undefined symbol` warnings
        # (-> kconfig errors -> twister CMake build failure).  The real
        # DT-overlay + runtime-enable plumbing lands per silicon in v0.7;
        # until then the wake-source list is documented in the generated
        # alp.conf for the customer to wire by hand.
        if isinstance(wake, str) and not wake.startswith("E1M_"):
            pwr_lines.append(
                f"# wakeup source: {wake} "
                "(DT wakeup-source; + pm_device_wakeup_enable() pending v0.7)")
        elif isinstance(wake, str):
            pwr_lines.append(f"# wakeup source: {wake} (per-silicon Kconfig pending)")
    if pwr_lines:
        lines.append("# Per-slice power-management profile (board.yaml cores.<id>.power:)")
        lines.extend(pwr_lines)
        lines.append("")

    # ----------------------------------------------------------------
    # Storage partitions (board.yaml `storage:` block).  Emits per-fs
    # Kconfig for every resolved (non-blocked) partition.  Blocked
    # partitions produce a hint comment so the slice build still
    # compiles but the runtime mount fails loudly via the DTS overlay.
    #
    # Per-slice ownership of partitions is not modelled today: every
    # Zephyr/baremetal slice gets the union.  Kconfig dedupes when
    # multiple per-core fragments overlay onto the same base.  v0.7
    # may add `core:` to storage entries for per-slice scoping.
    # ----------------------------------------------------------------
    partitions = resolve_storage_partitions(project)
    ok_partitions = [p for p in partitions if p.status == "ok"]
    fs_seen: set[str] = set()
    for p in ok_partitions:
        fs_seen.add(p.fs)
    fs_kconfig: list[str] = []
    if "littlefs" in fs_seen:
        fs_kconfig.append("CONFIG_FILE_SYSTEM=y")
        fs_kconfig.append("CONFIG_FILE_SYSTEM_LITTLEFS=y")
    if "fat" in fs_seen:
        fs_kconfig.append("CONFIG_FILE_SYSTEM=y")
        fs_kconfig.append("CONFIG_FAT_FILESYSTEM_ELM=y")
    if "ext4" in fs_seen:
        fs_kconfig.append("CONFIG_FILE_SYSTEM=y")
        fs_kconfig.append("CONFIG_FILE_SYSTEM_EXT2=y")
    if fs_kconfig or ok_partitions:
        lines.append("# Storage partitions (board.yaml `storage:`)")
        # FILE_SYSTEM=y appears twice in the list when multiple fs
        # types are enabled; dedupe before emit.
        for kc in sorted(set(fs_kconfig)):
            lines.append(kc)
        # Per-partition LITTLEFS partition labels surface as a hint
        # comment only.  Modern Zephyr does NOT define a per-partition
        # CONFIG_FS_LITTLEFS_PARTITION_<NAME> Kconfig -- the partition
        # gets matched via the DT `fixed-partitions` node + the
        # chosen `zephyr,storage-partition` / FIXED_PARTITION_ID()
        # macro at runtime.  Emitting a Kconfig stem trips Zephyr's
        # "assignment to undefined symbol" warning + aborts the build.
        for p in ok_partitions:
            if p.fs == "littlefs":
                lines.append(
                    f"# partition[{p.name}] -> mount at runtime via "
                    f"FIXED_PARTITION_ID({p.name}_partition)")
        blocked = [p for p in partitions if p.status == "blocked"]
        for p in blocked:
            lines.append(f"# BLOCKED storage[{p.name}]: "
                         f"{p.reason or 'unknown reason'}")
        lines.append("")

    # ----------------------------------------------------------------
    # OTA Zephyr client (board.yaml `ota:` block, provider-driven dispatch).
    #
    # Resolved design Q on the Zephyr-side OTA client (ADR 0009 follow-up):
    #   provider: mender   -> Mender-MCU-client (out-of-tree, BSD-3)
    #   provider: hawkbit  -> Zephyr Hawkbit DDI client (upstream)
    #   provider: mcumgr   -> Zephyr MCUmgr (upstream; transport is the app's call)
    #
    # Yocto slices keep the existing Mender server-mode dispatch in
    # _slice_local_conf().  This block handles the Zephyr side: per-slice
    # Kconfig that compiles the matching client in.  Settings (server URL,
    # poll interval, tenant token) thread through Kconfig string values
    # when declared in `ota:`; placeholders (${VAR}) pass through verbatim
    # so the build system substitutes at link time.
    # ----------------------------------------------------------------
    ota = project.ota or {}
    provider = (ota.get("provider") or "").lower() if isinstance(ota, dict) else ""
    if provider and provider in ("mender", "hawkbit", "mcumgr"):
        lines.append(f"# OTA Zephyr client (board.yaml `ota.provider: {provider}`)")
        srv = (ota.get("server") or {}) if isinstance(ota.get("server"), dict) else {}
        poll = ota.get("poll_interval_s")
        if provider == "mender":
            # Mender-MCU-client.  The west.yml `mender` group is NOT
            # active by default (the upstream mender-mcu-client
            # repository is pinned alongside the v0.7 OTA wiring), so
            # CONFIG_MENDER_* would resolve to undefined-symbol warnings
            # under twister and abort the build.  Emit the settings as
            # hint comments so the configuration is visible to the
            # customer (and to anyone reviewing the generated alp.conf)
            # while keeping the build clean.  When the mender west
            # group flips active the comments here go back to live
            # CONFIG_* lines in a single commit.
            lines.append("# Mender-MCU-client wiring is pending the v0.7 OTA module")
            lines.append("# (mender-mcu-client west group activation).")
            lines.append("# CONFIG_MENDER_MCU_CLIENT=y")
            if srv.get("url"):
                lines.append(f'# CONFIG_MENDER_SERVER_URL="{srv["url"]}"')
            if srv.get("tenant"):
                lines.append(f'# CONFIG_MENDER_TENANT_TOKEN="{srv["tenant"]}"')
            if ota.get("artifact_name"):
                lines.append(f'# CONFIG_MENDER_ARTIFACT_NAME="{ota["artifact_name"]}"')
            if isinstance(poll, int) and poll > 0:
                lines.append(f"# CONFIG_MENDER_UPDATE_POLL_INTERVAL={poll}")
        elif provider == "hawkbit":
            # Zephyr upstream's Hawkbit DDI client.
            lines.append("CONFIG_HAWKBIT=y")
            lines.append("CONFIG_HAWKBIT_SHELL=y")
            if srv.get("url"):
                lines.append(f'CONFIG_HAWKBIT_SERVER="{srv["url"]}"')
            if isinstance(poll, int) and poll > 0:
                lines.append(f"CONFIG_HAWKBIT_POLL_INTERVAL={poll}")
        elif provider == "mcumgr":
            # MCUmgr is upstream; the transport (UART/BLE/UDP) stays the
            # app's call -- we only enable the base SMP server.
            lines.append("CONFIG_MCUMGR=y")
            lines.append("CONFIG_MCUMGR_GRP_IMG=y")
            lines.append("CONFIG_MCUMGR_GRP_OS=y")
            lines.append("# MCUmgr transport (UART/BLE/UDP) is the app's call;")
            lines.append("# enable the matching CONFIG_MCUMGR_TRANSPORT_* in your prj.conf.")
        lines.append("")

    # ----------------------------------------------------------------
    # Per-module log levels (board.yaml `diagnostics.modules:`).
    #
    # Zephyr's per-module CONFIG_<MOD>_LOG_LEVEL symbol exists only when
    # the matching module has called LOG_MODULE_REGISTER() at build time.
    # ALP_* SDK-side modules have not registered any LOG modules yet, so
    # emitting `CONFIG_ALP_<MOD>_LOG_LEVEL=N` trips the Zephyr build with
    # `undefined symbol ALP_<MOD>_LOG_LEVEL`.  We restrict the active
    # emit to modules whose Kconfig declaration is known to exist and
    # downgrade ALP_* + unknown modules to a hint comment until their
    # LOG_MODULE_REGISTER call lands.
    # ----------------------------------------------------------------
    modules = (project.diagnostics or {}).get("modules") or {}
    if modules:
        level_to_n = {"off": 0, "error": 1, "warn": 2, "info": 3, "debug": 4, "trace": 4}
        lines.append("# Per-module log-level overrides (board.yaml diagnostics.modules:)")
        for mod, lvl in sorted(modules.items()):
            n = level_to_n.get(str(lvl).lower())
            if n is None:
                continue
            kc_stem = mod.upper()
            if kc_stem.startswith("ALP_"):
                lines.append(
                    f"# CONFIG_{kc_stem}_LOG_LEVEL={n} "
                    "(pending LOG_MODULE_REGISTER on this SDK module)")
            else:
                lines.append(f"CONFIG_{kc_stem}_LOG_LEVEL={n}")
        lines.append("")

    return "\n".join(lines) + "\n"


def _slice_local_conf(project: BoardProject, slice_: Slice) -> str:
    """Per-core local.conf snippet for a Yocto slice."""
    machine = slice_.machine or f"e1m-{project.sku.lower().replace('e1m-', '')}"
    lines: list[str] = []
    lines.append("# Auto-generated by scripts/alp_orchestrate.py "
                 "-- append to local.conf.")
    lines.append(f"# Per-core slice `{slice_.core_id}` "
                 f"(image: {slice_.image or 'custom'})")
    lines.append(f'MACHINE = "{machine}"')
    if slice_.libraries:
        imageinstall = " ".join(
            f"lib-{lib.replace('_', '-')}" for lib in slice_.libraries)
        lines.append(f'IMAGE_INSTALL:append = " {imageinstall}"')
    if slice_.image:
        lines.append(f"# bitbake target: {slice_.image}")

    # Mender OTA wiring (board.yaml `ota:` block).  Emits Mender
    # `?=` (weak) assignments so hand-edited local.conf values in
    # the build dir still win.
    ota = project.ota or {}
    if ota.get("provider") == "mender":
        lines.append("")
        lines.append("# OTA (board.yaml `ota:` block)")
        lines.append('INHERIT += "mender-full"')
        if ota.get("artifact_name"):
            lines.append(f'MENDER_ARTIFACT_NAME ?= "{ota["artifact_name"]}"')
        srv = ota.get("server") or {}
        if srv.get("url"):
            lines.append(f'MENDER_SERVER_URL ?= "{srv["url"]}"')
        if srv.get("tenant"):
            lines.append(f'MENDER_TENANT_TOKEN ?= "{srv["tenant"]}"')
        sto = ota.get("storage") or {}
        if sto.get("device"):
            lines.append(f'MENDER_STORAGE_DEVICE_BASE ?= "{sto["device"]}"')
        if sto.get("boot_part_mb"):
            lines.append(f'MENDER_BOOT_PART_SIZE_MB ?= "{sto["boot_part_mb"]}"')
        if sto.get("total_size_mb"):
            lines.append(f'MENDER_STORAGE_TOTAL_SIZE_MB ?= "{sto["total_size_mb"]}"')
        if ota.get("poll_interval_s"):
            lines.append(f'MENDER_INVENTORY_INTERVAL ?= "{ota["poll_interval_s"]}"')

    return "\n".join(lines) + "\n"


def emit_sysbuild_conf(project: BoardProject) -> str:
    """Emit the sysbuild.conf overlay for the project's `boot:` block.

    Returns a SB_CONFIG_*-style snippet the build wires into
    `build/alp_sysbuild.conf` (consumed via `west build --sysbuild
    --sysbuild-config`).  Returns an empty string when the project
    does not declare a `boot:` block (the SDK's stock per-family
    defaults at zephyr/sysbuild/<family>/sysbuild.conf apply).
    """
    boot = project.boot or {}
    if not boot:
        return ""
    method = (boot.get("method") or "mcuboot").lower()
    if method == "none":
        return ("# Auto-generated from board.yaml `boot:` block.\n"
                "SB_CONFIG_BOOTLOADER_MCUBOOT=n\n")
    if method != "mcuboot":
        return ""

    lines: list[str] = [
        "# Auto-generated by scripts/alp_orchestrate.py from "
        "board.yaml `boot:`.",
        "# Drives the sysbuild MCUboot child image.  Customers who",
        "# omit `boot:` get the SDK's stock per-family defaults.",
        "",
        "SB_CONFIG_BOOTLOADER_MCUBOOT=y",
    ]
    sign = boot.get("signing") or {}
    algo = (sign.get("algorithm") or "").lower()
    algo_kc = {
        "ecdsa_p256": "SB_CONFIG_MCUBOOT_SIGNATURE_TYPE_ECDSA_P256=y",
        "rsa2048":    "SB_CONFIG_MCUBOOT_SIGNATURE_TYPE_RSA=y\n"
                      "SB_CONFIG_MCUBOOT_SIGNATURE_TYPE_RSA_LEN=2048",
        "rsa3072":    "SB_CONFIG_MCUBOOT_SIGNATURE_TYPE_RSA=y\n"
                      "SB_CONFIG_MCUBOOT_SIGNATURE_TYPE_RSA_LEN=3072",
        "ed25519":    "SB_CONFIG_MCUBOOT_SIGNATURE_TYPE_ED25519=y",
    }.get(algo)
    if algo_kc:
        lines.append(algo_kc)
    if sign.get("key_file"):
        lines.append(f'SB_CONFIG_BOOT_SIGNATURE_KEY_FILE="{sign["key_file"]}"')
    slots = boot.get("slots") or {}
    primary = (slots.get("primary") or {}).get("size_kib")
    secondary = (slots.get("secondary") or {}).get("size_kib")
    if primary:
        lines.append(f"SB_CONFIG_BOOT_PRIMARY_PARTITION_SIZE={int(primary) * 1024}")
    if secondary:
        lines.append(f"SB_CONFIG_BOOT_SECONDARY_PARTITION_SIZE={int(secondary) * 1024}")
    swap = (boot.get("swap_algorithm") or "scratch").lower()
    swap_kc = {
        "scratch":   "SB_CONFIG_MCUBOOT_MODE_SWAP_USING_SCRATCH=y",
        "move":      "SB_CONFIG_MCUBOOT_MODE_SWAP_USING_MOVE=y",
        "overwrite": "SB_CONFIG_MCUBOOT_MODE_OVERWRITE_ONLY=y",
    }.get(swap)
    if swap_kc:
        lines.append(swap_kc)
    if swap == "scratch" and boot.get("scratch_size_kib"):
        lines.append(f"SB_CONFIG_BOOT_SCRATCH_PARTITION_SIZE="
                     f"{int(boot['scratch_size_kib']) * 1024}")
    if boot.get("anti_rollback"):
        lines.append("SB_CONFIG_BOOT_COUNTERS_MCUBOOT=y")
    return "\n".join(lines) + "\n"


def emit_tfm_sysbuild_conf(project: BoardProject) -> str:
    """Emit the TF-M sysbuild child-image overlay for `security.psa:`.

    Returns a `SB_CONFIG_*` + `CONFIG_PSA_CRYPTO_*` snippet the build
    wires into `build/sysbuild/tfm/tfm.conf` (consumed via
    `west build --sysbuild --sysbuild-config`).  Returns an empty
    string when the project omits `security.psa:` or sets
    `tfm: false` -- PSA Crypto then runs entirely non-secure
    (mbedTLS-only) and no child image is built.

    The TF-M boundary lives on the same M55-HP core as the non-secure
    app via Armv8-M TrustZone-M; see ADR 0013.
    """
    security = project.security or {}
    psa = security.get("psa") or {}
    if not psa or not psa.get("tfm"):
        return ""

    # TF-M build type follows the project's MCUboot build_type when
    # set so the secure + bootloader artefacts ship the same flavour.
    # Default is Release; debug builds use MinSizeRel for parity with
    # the upstream sysbuild template.
    boot = project.boot or {}
    build_type = (boot.get("build_type") or "Release")
    if build_type.lower() == "debug":
        tfm_build_type = "Debug"
    elif build_type.lower() in ("minsizerel", "min_size_rel"):
        tfm_build_type = "MinSizeRel"
    elif build_type.lower() == "release":
        tfm_build_type = "Release"
    else:
        # Pass through verbatim if the customer set an exact CMake
        # build-type string we don't pre-canonicalise.
        tfm_build_type = build_type

    lines: list[str] = [
        "# Auto-generated by scripts/alp_orchestrate.py from "
        "board.yaml `security.psa:`.",
        "# Drives the sysbuild TF-M child image on the M55-HP core via "
        "Armv8-M TrustZone-M.",
        "# See docs/adr/0013-tfm-boundary-m55-hp-trustzone.md and "
        "docs/security-audit-plan.md.",
        "",
        "SB_CONFIG_TFM=y",
        f"SB_CONFIG_TFM_BUILD_TYPE={tfm_build_type}",
    ]

    persistent_slots = psa.get("persistent_slots")
    if persistent_slots is None:
        # Schema default mirrors v0.6: 16 slots.
        persistent_slots = 16
    lines.append(
        f"CONFIG_PSA_CRYPTO_PERSISTENT_SLOT_COUNT={int(persistent_slots)}"
    )

    its = psa.get("its_storage")
    if its:
        lines.append(f'CONFIG_PSA_CRYPTO_ITS_BACKING_STORE="{its}"')
    ps = psa.get("ps_storage")
    if ps:
        lines.append(f'CONFIG_PSA_CRYPTO_PS_BACKING_STORE="{ps}"')

    att_root = (psa.get("attestation_root") or "none").lower()
    if att_root == "optiga_trust_m":
        lines.append(
            "# Attestation root anchored in OPTIGA Trust M (on-module "
            "secure element).")
        lines.append(
            "# See src/security/optiga_trust_m_bridge.c for the "
            "PSA <-> OPTIGA bridge driver.")
        lines.append("CONFIG_ALP_SDK_PSA_ATTESTATION_OPTIGA=y")
    elif att_root == "tfm_internal":
        lines.append("CONFIG_ALP_SDK_PSA_ATTESTATION_TFM_INTERNAL=y")

    return "\n".join(lines) + "\n"


def _slice_cmake_args(project: BoardProject, slice_: Slice) -> str:
    """Per-core cmake -D args for a baremetal / yocto slice.

    NPU-dispatch enables (DRP-AI, DEEPX) are driven from the SoM
    preset's `capabilities:` matrix -- never from board.yaml.  On
    multi-NPU SKUs (V2M101 = DRP-AI3 + DEEPX DX-M1) every available
    NPU is enabled so apps can dispatch concurrent independent models
    via alp_inference_open(.backend=...) per-handle.
    """
    family = project.som_preset.get("family") or "unknown"
    # resolve_capabilities merges SoC-JSON defaults with SoM overrides.
    capabilities = resolve_capabilities(project.som_preset, METADATA_ROOT)
    lines: list[str] = []
    lines.append("# Auto-generated by scripts/alp_orchestrate.py "
                 "-- pass to cmake.")
    lines.append(f"-DALP_SOM_SKU={project.sku}")
    lines.append(f"-DALP_SOM_FAMILY={family}")
    lines.append(f"-DALP_CORE_ID={slice_.core_id}")
    if slice_.toolchain:
        lines.append(f"-DALP_TOOLCHAIN={slice_.toolchain}")
    if capabilities.get("drp_ai"):
        lines.append("-DALP_SDK_USE_DRPAI=ON")
    if capabilities.get("deepx_dx"):
        lines.append("-DALP_SDK_USE_DEEPX_DXM1=ON")
    return "\n".join(lines) + "\n"


_PERIPHERAL_KCONFIG: dict[str, str] = {
    "adc":      "ADC",
    "can":      "CAN",
    "counter":  "COUNTER",
    "gpio":     "GPIO",
    "i2c":      "I2C",
    "i2s":      "I2S",
    "pwm":      "PWM",
    "rtc":      "RTC",
    "sensor":   "SENSOR",
    "spi":      "SPI",
    "uart":     "SERIAL",
    "watchdog": "WATCHDOG",
}


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


def _slice_command(
    project: BoardProject,
    slice_: Slice,
) -> Optional[list[str]]:
    """Resolve the build command for a slice.  Returns None when
    Phase 2 has no command for this os yet (caller marks the slice
    `skipped`)."""
    if slice_.os == "zephyr":
        if not slice_.app or not slice_.board:
            return None
        return [
            "west", "build",
            "-b", slice_.board,
            str(_resolve_app_path(slice_.app)),
        ]
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


# ---------------------------------------------------------------------
# CLI (thin wrapper for ad-hoc invocation)
# ---------------------------------------------------------------------


def main(argv: Optional[Iterable[str]] = None) -> int:
    import argparse
    parser = argparse.ArgumentParser(
        description="Fan-out orchestrator for board.yaml.")
    parser.add_argument("--input", type=Path, default=Path("board.yaml"),
                        help="Path to the project's board.yaml.")
    parser.add_argument("--build-root", type=Path,
                        default=Path("build"),
                        help="Build root directory.")
    parser.add_argument("--core", default=None,
                        help="Limit fan-out to a single core ID.")
    parser.add_argument("--no-parallel", action="store_true",
                        help="Force sequential dispatch.")
    parser.add_argument("--emit", default=None,
                        choices=["system-manifest", "ipc-contract-h",
                                 "dts-reservations", "dts-partitions",
                                 "storage-mounts-c",
                                 "tfm-sysbuild-conf"],
                        help="Skip the build; just emit one of the "
                             "generated artefacts to stdout.")
    args = parser.parse_args(list(argv) if argv is not None else None)

    try:
        project = load_board_yaml(args.input)
    except OrchestratorError as e:
        print(f"alp-orchestrate: {e}", file=sys.stderr)
        return 1

    if args.emit:
        try:
            if args.emit == "system-manifest":
                sys.stdout.write(emit_system_manifest(project))
            elif args.emit == "ipc-contract-h":
                sys.stdout.write(emit_ipc_contract_h(project))
            elif args.emit == "dts-reservations":
                sys.stdout.write(emit_dts_reservations(project))
            elif args.emit == "dts-partitions":
                sys.stdout.write(emit_dts_partitions(project))
            elif args.emit == "storage-mounts-c":
                sys.stdout.write(emit_storage_mounts_c(project))
            elif args.emit == "tfm-sysbuild-conf":
                sys.stdout.write(emit_tfm_sysbuild_conf(project))
        except OrchestratorError as e:
            print(f"alp-orchestrate: {e}", file=sys.stderr)
            return 1
        return 0

    orchestrator = Orchestrator(project, args.build_root)
    try:
        manifest = orchestrator.fan_out(
            only_core=args.core, parallel=not args.no_parallel)
    except OrchestratorError as e:
        print(f"alp-orchestrate: {e}", file=sys.stderr)
        return 1

    # Surface per-slice status to the console.
    failed = 0
    for s in manifest.slices:
        marker = {
            "ok":      "[OK ]",
            "failed":  "[FAIL]",
            "skipped": "[SKIP]",
            "pending": "[??? ]",
        }.get(s.status, "[??? ]")
        extra = f" -- {s.reason}" if s.reason else ""
        print(f"{marker} {s.core_id}/{s.os}{extra}")
        if s.status == "failed":
            failed += 1
    print(f"alp-orchestrate: manifest at "
          f"{(args.build_root / 'system-manifest.yaml')}")
    return 1 if failed > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
