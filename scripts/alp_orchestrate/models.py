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

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

from .paths import METADATA_ROOT


class OrchestratorError(RuntimeError):
    """Raised when the orchestrator can't resolve / build a project.

    Carries a human-readable message; the caller (west wrapper / CI)
    prints it and exits non-zero.
    """


class SdkRevisionUnsupported(OrchestratorError):
    """The running SDK is outside a requested hw_rev's declared range.

    A subclass rather than a flag so `scripts/validate_board_yaml.py` can
    map exactly this failure to the **exit code 3** that
    `metadata/sdk_version.yaml` documents, without string-matching a
    message.  Every existing `except OrchestratorError` keeps catching it
    (#1019).
    """


class SdkRevisionUnknown(OrchestratorError):
    """The requested hw_rev is not a key in its resolved `hw_revisions:` table.

    Distinct from `SdkRevisionUnsupported`: that one names a revision that
    exists but whose declared SDK range excludes the running SDK.  This one
    names a revision that doesn't exist at all -- there is no range to
    compare against, so reporting it as out-of-range would name the wrong
    cause.  A subclass (not a flag) for the same reason as
    `SdkRevisionUnsupported`: `scripts/validate_board_yaml.py` maps exactly
    this failure to its own exit code without string-matching a message,
    and every existing `except OrchestratorError` keeps catching it (#1025,
    the existence-only half).
    """


class SdkRevisionNotBuildable(OrchestratorError):
    """The requested hw_rev exists but its declared `status:` refuses a build.

    Distinct from `SdkRevisionUnknown`: that one names a revision that
    isn't a key in its table at all.  This one names a revision that IS a
    key -- it exists -- but is `status: reserved`, `status: tbd`, or
    carries no `status` key at all (the maintainer's broad-reading decision
    on #1025's status half).  Its own exit code in
    `scripts/validate_board_yaml.py` for the same mechanical-action reason
    as the other two: "exists but is not buildable" needs a different fix
    ("pick a different revision" once one is buildable) than "does not
    exist" ("pick a revision that exists") or "SDK out of range" ("pin a
    different SDK").
    """


_E1M_I2C_BUS_RE = re.compile(r"^e1m(?:_x)?_i2c([0-9]+)$")


def _feature_i2c_bus_id(bus: Any) -> int:
    if not isinstance(bus, str):
        raise OrchestratorError(
            "features.hw_info.eeprom.bus must be an E1M I2C bus slug "
            "such as `e1m_i2c0`.")
    match = _E1M_I2C_BUS_RE.fullmatch(bus)
    if match is None:
        raise OrchestratorError(
            f"features.hw_info.eeprom.bus: {bus!r} is not supported; "
            "use an E1M I2C bus slug such as `e1m_i2c0`.")
    return int(match.group(1), 10)


def _feature_non_negative_int(raw: dict[str, Any], key: str) -> int:
    value = raw.get(key)
    if not isinstance(value, int) or value < 0:
        raise OrchestratorError(
            f"features.hw_info.eeprom.{key} must be a non-negative integer.")
    return value


def _normalise_hw_info_eeprom(raw: dict[str, Any], source: str) -> dict[str, Any]:
    return {
        "source":    source,
        "bus":       raw.get("bus"),
        "bus_id":    _feature_i2c_bus_id(raw.get("bus")),
        "addr_7bit": _feature_non_negative_int(raw, "addr_7bit"),
        "offset":    _feature_non_negative_int(raw, "offset"),
    }


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
    # Yocto bitbake recipe that packages this slice's `app:` source dir.
    # Required for an app-only (`app:` set, no `image:`) Yocto slice --
    # `app:` is a filesystem path, never a valid bitbake target itself
    # (issue #597).
    recipe: Optional[str] = None
    machine: Optional[str] = None        # Yocto MACHINE
    board: Optional[str] = None          # Zephyr board target
    toolchain: Optional[str] = None
    peripherals: list[str] = field(default_factory=list)
    libraries: list[str] = field(default_factory=list)
    # Open-set escape hatch for libraries the SDK doesn't curate.  Each
    # entry is a dict with `name:` + (exclusively) `kconfig:` OR
    # `profile:`; loader's _validate_consistency() enforces the
    # exactly-one and uniqueness rules.  See docs/board-config-schema.md
    # `extra_libraries:`.
    extra_libraries: list[dict[str, Any]] = field(default_factory=list)
    inference: dict[str, Any] = field(default_factory=dict)
    iot: dict[str, Any] = field(default_factory=dict)
    memory: dict[str, Any] = field(default_factory=dict)   # stack_kib, heap_kib, isr_stack_kib
    power: dict[str, Any] = field(default_factory=dict)    # sleep_mode, wakeup_sources
    # False when this core has no hardware UART console (headless -- e.g.
    # the RZ/V2N M33 system-manager, whose debug UART the A55 owns).  A
    # SoM-topology fact (`topology.<id>.hw_console:` in the SoM preset),
    # NOT customer-overridable; defaults True so every other core keeps
    # its console.  Consumed by the console emitter for `diagnostics.
    # sim_console:` (issue #686).
    hw_console: bool = True
    # SEGGER J-Link **part-number flash-device** profile for this slice's
    # SoC variant (soc-spec-v1 `variants[].debug.jlink_flash_device`) --
    # unlocks the built-in Alif MRAM loader (Flow D), distinct from the
    # generic attach/debug `jlink_device` profile.  A resolved SoC-variant
    # fact, like `hw_console` above, NOT customer-overridable; None when
    # the variant publishes no such profile -- a published "unknown", not
    # a gap to guess at (see soc-spec-v1.schema.json's `jlink_flash_device`
    # description).  Consumed by `_slice_flash_recipe`'s `zephyr` branch to
    # arm the direct-flash path in `flash_args`.
    jlink_flash_device: Optional[str] = None
    # Whether the variant's `debug:` block DECLARED `jlink_flash_device` at
    # all, independent of its value (#1295 / tan-cli#734).  `dict.get`
    # returns None both for a schema-declared `jlink_flash_device: null` and
    # for an absent key, and those mean OPPOSITE things: a declared null is
    # a published "no known J-Link flash profile -- refuse loudly" (e4.json
    # is the one real case), while absent means the variant says nothing and
    # the Flow A default stands.  The downstream contract is presence-based
    # (`flash_plan._fa_has_key` in tan), so the emitter must not collapse the
    # two -- see `_slice_flash_recipe`.
    jlink_flash_device_declared: bool = False
    # The read-only SW-DP IDR (DPIDR) wrong-board preflight PAIR for this
    # core (soc-spec-v1 `variants[].debug.expect_dpidr` +
    # `variants[].debug.jlink_device[<core_id>]`), resolved together by
    # `loader._resolve_flow_d_preflight` -- both None (preflight not armed,
    # every variant that has not been measured) or both set; never one of
    # the two.  `expect_dpidr` is the ID the board's debug port must answer
    # BEFORE any write; `jlink_device` is the LIVE-CORE attach profile that
    # read is performed with -- distinct from `jlink_flash_device` above,
    # which is the part-number flash-algorithm profile.  Resolved SoC-variant
    # facts like `jlink_flash_device`, NOT customer-overridable.  Consumed by
    # `_slice_flash_recipe`'s `zephyr` branch, which emits them into
    # `flash_args` as an inseparable pair (#1355).
    expect_dpidr: Optional[str] = None
    jlink_device: Optional[str] = None
    # This core's AEN MRAM slot0-XIP load address, `0x`-prefixed hex string
    # (tan-cli#353) -- where Flow D's built-in Alif MRAM loader must write
    # the slot0-linked application blob itself, distinct from
    # `jlink_flash_device` above (which only selects the loader's device
    # PROFILE, not an address). Resolved by
    # `loader._resolve_slot0_load_address` from the SoM preset's
    # `memory_map:` (NOT the SoC JSON -- this is SDK/module build policy,
    # not a silicon fact: metadata/e1m_modules/E1M-AEN801.yaml's own
    # `memory_map:` comment says so explicitly, #1069), so like
    # `jlink_flash_device` it is NOT customer-overridable.  None when this
    # core has no AEN slot0-XIP window (every non-AEN slice, and any AEN
    # core whose SoC variant publishes no `jlink_flash_device`) -- a
    # published "unknown", never a value to invent. Consumed by
    # `_slice_flash_recipe`'s `zephyr` branch.
    slot0_load_address: Optional[str] = None

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
            "recipe":           self.recipe,
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
    # Project-wide curated third-party libraries (ADR 0018).  Each name
    # resolves to metadata/libraries/<name>.yaml; the emitters wire its
    # per-OS integration into every slice whose OS the manifest supports.
    libraries: list[str] = field(default_factory=list)
    features: dict[str, Any] = field(default_factory=dict)
    boot: dict[str, Any] = field(default_factory=dict)
    ota: dict[str, Any] = field(default_factory=dict)
    storage: list[StorageEntry] = field(default_factory=list)
    security: dict[str, Any] = field(default_factory=dict)
    raw: dict[str, Any] = field(default_factory=dict)
    # The metadata root `load_board_yaml(..., metadata_root=...)` resolved
    # this project against.  `None` means the default in-tree `metadata/`
    # (the common case); an explicit override (`--metadata-root <path>`)
    # sets it so every post-load resolver reads the SAME tree the loader
    # validated against, instead of quietly falling back to the SDK's own
    # in-tree metadata (#1485).
    metadata_root: Optional[Path] = None

    def effective_metadata_root(self) -> Path:
        """The metadata root every resolver must use for this project --
        the explicit override if `load_board_yaml` was given one, else the
        SDK's own in-tree `metadata/`."""
        return (self.metadata_root if self.metadata_root is not None
                else METADATA_ROOT)

    def hw_info_eeprom_feature(self) -> Optional[dict[str, Any]]:
        """Return the explicit ``features.hw_info.eeprom`` projection."""
        hw_info = self.features.get("hw_info")
        if not isinstance(hw_info, dict):
            return None
        eeprom = hw_info.get("eeprom")
        if eeprom is None:
            return None
        if not isinstance(eeprom, dict):
            raise OrchestratorError(
                "features.hw_info.eeprom must be a mapping.")
        return _normalise_hw_info_eeprom(eeprom, "features.hw_info.eeprom")

    def hw_info_eeprom_config(self) -> Optional[dict[str, Any]]:
        """Return the effective EEPROM reader config for Kconfig emit."""
        explicit = self.hw_info_eeprom_feature()
        if explicit is not None:
            return explicit
        if (self.som_preset.get("on_module") or {}).get("eeprom"):
            return {
                "source":    "som.on_module.eeprom",
                "bus":       "e1m_i2c0",
                "bus_id":    0,
                "addr_7bit": 0x50,
                "offset":    0,
            }
        return None


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
        hw_info: dict[str, Any] = {
            "sku":          self.project.sku,
            "som_hw_rev":   self.project.hw_rev,
            "board_name":   self.project.board_name,
            "board_hw_rev": self.project.board_hw_rev,
            "silicon":      self.project.som_preset.get("silicon"),
        }
        eeprom = self.project.hw_info_eeprom_feature()
        if eeprom is not None:
            hw_info["eeprom"] = {
                "bus":       eeprom["bus"],
                "bus_id":    eeprom["bus_id"],
                "addr_7bit": eeprom["addr_7bit"],
                "offset":    eeprom["offset"],
            }

        out: dict[str, Any] = {
            "schema_version": 1,
            "generated_by":   "scripts/alp_orchestrate.py",
            "hw_info":        hw_info,
            "slices":      [s.to_manifest_entry() for s in self.slices],
            "ipc":         [c.to_manifest_entry() for c in self.carve_outs],
            "helper_mcus": list(self.helper_mcus),
            "boot_order":  list(self.boot_order),
        }
        if self.partitions:
            out["storage"] = [p.to_manifest_entry() for p in self.partitions]
        return out
