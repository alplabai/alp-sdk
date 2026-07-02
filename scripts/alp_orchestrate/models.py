# SPDX-License-Identifier: Apache-2.0
#
# Orchestrator data model — the dataclasses the fan-out resolves into and the
# emitters serialise from.  Extracted verbatim from alp_orchestrate.py as the
# first step of the #285 modularization (one seam, no behaviour change): these
# are pure data + their own serialisation, with no resolver/emitter logic, so
# they make a clean leaf module that alp_orchestrate.py (and, later,
# alp_project.py) re-exports.  Public names are unchanged — `from alp_orchestrate
# import Slice` still works because alp_orchestrate re-exports from here.
"""Dataclasses for the board.yaml orchestrator."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional


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
        # Local import: the flash-recipe deriver lives in alp_orchestrate; a
        # module-level import here would create a models<->orchestrate cycle.
        # By call time both modules are loaded, so this resolves cleanly.
        from alp_orchestrate.orchestrator import _slice_flash_recipe

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
