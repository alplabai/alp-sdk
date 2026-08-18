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
from alp_project import resolve_memory_map, resolve_soc_path
from sentinels import is_tbd
from strict_loaders import DuplicateKeyError, strict_json_loads, strict_yaml_load

from . import sdk_compat
from .models import (BoardProject, IpcEntry, OrchestratorError,
                     SdkRevisionNotBuildable, SdkRevisionUnknown,
                     SdkRevisionUnsupported, Slice, StorageEntry)
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
    soc_path = resolve_soc_path(silicon, metadata_root)
    if soc_path is None:
        raise OrchestratorError(
            f"silicon ref '{silicon}' is not a triple-colon string")
    return soc_path


# ---------------------------------------------------------------------
# Loader
# ---------------------------------------------------------------------


def _load_yaml(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise OrchestratorError(f"file not found: {path}")
    try:
        data = strict_yaml_load(path.read_text(encoding="utf-8"), source=path)
    except (yaml.YAMLError, DuplicateKeyError) as e:
        raise OrchestratorError(f"failed to parse {path}: {e}") from e
    if not isinstance(data, dict):
        raise OrchestratorError(
            f"{path} did not parse to a top-level mapping")
    return data


def _load_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise OrchestratorError(f"file not found: {path}")
    try:
        return strict_json_loads(path.read_text(encoding="utf-8"), source=path)
    except (json.JSONDecodeError, DuplicateKeyError) as e:
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


def _resolve_variant_debug(
    som_preset: dict[str, Any],
    soc_spec: dict[str, Any],
) -> dict[str, Any]:
    """Resolve the whole `variants[].debug` block for the SoC variant this
    SoM preset declares -- `{}` when the variant can't be resolved or
    publishes no `debug:` block at all.

    Matches the SoM's `silicon_variant:` against `soc_spec["variants"][].
    order_code`, falling back to `alp_module_skus` membership when
    `silicon_variant:` is absent/"TBD" -- the same two-step match as the
    canonical `alp_project_loader._resolve_silicon_variant` (re-exported as
    `alp_project._resolve_silicon_variant`) and `gen_zephyr_board.
    _resolve_variant`.  NOT delegated to that canonical helper -- unlike
    `resolve_memory_map` (imported from `alp_project` two lines up in this
    same file, so a cross-import is clearly not the obstacle), it takes
    `(sku_preset, metadata_root)` and re-resolves the SoC-JSON path + reads
    + re-parses it from disk itself.  By the time this function runs,
    `_resolve_board_impl` has already loaded that exact JSON into the
    `soc_spec` dict this function receives as a parameter, and this
    function's callers have no other use for `metadata_root`.  Delegating
    would mean a second disk read + parse of a file already in hand, purely
    to save this six-line match -- kept independent instead, deliberately in
    lockstep with both of the above; if either one grows a third fallback or
    drops the "TBD" sentinel, update this one too.

    ONE variant match shared by every `debug:` reader below
    (`_resolve_jlink_flash_device`, `_resolve_flow_d_preflight`) so a future
    third fact cannot drift onto a differently-resolved variant than its
    siblings -- the keys are read as a set precisely because
    `expect_dpidr`/`jlink_device` are validated downstream as a PAIR.
    """
    variants = soc_spec.get("variants") or []
    variant: Optional[dict[str, Any]] = None
    declared = som_preset.get("silicon_variant")
    if declared and not is_tbd(declared):
        variant = next(
            (v for v in variants if v.get("order_code") == declared), None)
    if variant is None:
        sku = som_preset.get("sku")
        variant = next(
            (v for v in variants if sku in (v.get("alp_module_skus") or [])),
            None)
    if variant is None:
        return {}
    return dict(variant.get("debug") or {})


def _resolve_jlink_flash_device(
    debug: dict[str, Any],
) -> Optional[str]:
    """`debug.jlink_flash_device` out of an already-resolved `debug:` block
    (`_resolve_variant_debug`).

    `jlink_flash_device` is a single per-variant string (per
    soc-spec-v1.schema.json's own description: it "is not per-core... a
    single per-variant string, not a jlink_device-shaped map"), unlike its
    sibling `jlink_device` which IS keyed by core id -- so this returns one
    value reused for every core's slice, not a per-core lookup.

    None when the variant couldn't be resolved (`debug` is then `{}`), or
    the resolved variant's `debug:` block carries no `jlink_flash_device`
    key.  Those two are NOT the same fact -- a schema-declared
    `jlink_flash_device: null` is a published "no known J-Link flash
    profile" that consumers must refuse on, while an absent key means the
    variant says nothing.  Ask `_jlink_flash_device_declared` alongside this
    to tell them apart (#1295); never infer declaration from this value.
    """
    return debug.get("jlink_flash_device")


def _jlink_flash_device_declared(debug: dict[str, Any]) -> bool:
    """Whether the resolved `debug:` block DECLARES `jlink_flash_device`,
    whatever its value -- the fact `_resolve_jlink_flash_device` cannot
    carry, since a declared null and an absent key both resolve to None
    there.  Must be asked of `debug` directly, before the null collapses.
    """
    return "jlink_flash_device" in debug


def _resolve_slot0_load_address(
    som_preset: dict[str, Any], core_id: str,
) -> Optional[str]:
    """This core's AEN MRAM slot0-XIP load address, as a `0x`-prefixed hex
    string for `flash_args.slot0_load_address` (tan-cli#353): the Flow D
    built-in MRAM loader (`alif_mram_jlink`) needs to know where the
    slot0-linked application blob itself belongs, distinct from
    `jlink_flash_device` (which only picks the loader's *device profile*).
    Before this, alp-sdk emitted no such key at all -- tan correctly routed
    an AEN slice to Flow D and then refused outright (`flash_args.
    slot0_load_address is required to auto-sign via SETOOLS`), forcing a
    customer to hand-edit `system-manifest.yaml` to flash an AEN.

    Deliberately sourced from the SoM preset's `memory_map:`, NOT from the
    SoC JSON `debug:` block `jlink_flash_device` lives in: this address is
    SDK/module build POLICY, not a silicon fact --
    `metadata/e1m_modules/E1M-AEN801.yaml`'s own `memory_map:` comment says
    so explicitly (#1069), because two SoMs built on the same silicon part
    can freely choose different slot0 windows. That is the exact bug #1069
    fixed: a stock symmetric layout put HE's and HP's slot0 at the SAME
    address and flashing one silently clobbered the other.

    Reuses `scripts/gen_zephyr_board.py`'s own `_aen_role_slot0_map` (lazy
    import, matching the existing cross-module convention at
    `scripts/alp_project.py:279`) rather than re-deriving its per-role
    override lookup, so this and board generation read the identical
    override/no-override decision and genuinely cannot disagree on THAT
    part. A declared-but-invalid override (a half-authored map naming only
    one role's `<role>_slot0`, or a wrong `accessible_from`) makes
    `_aen_role_slot0_map` raise `ZephyrBoardEmitError` -- board generation
    refuses to build that core's board file for the same reason, so this
    re-raises as `OrchestratorError` instead of silently publishing an
    address no board was actually built for.

    Bench-proven at this exact address for the HE core (`0x80010000`):
    `docs/aen-bench-bringup.md`, `docs/aen-provisioning.md` §0.5, and
    `docs/secure-boot.md` all record a plain-J-Link `loadbin` straight to
    slot0 `0x80010000` -- no SETOOLS/ATOC/SE-UART -- verified by MCUboot,
    chainloaded, and surviving repeated cold power-cycles.

    Only `m55_he`/`m55_hp` roles are AEN slot0-XIP cores; every other
    core_id (a32_cluster, non-AEN SoC families) returns None -- a
    published "unknown", never a value to invent (same convention as
    `jlink_flash_device`'s absent key).

    No-override default: when `_aen_role_slot0_map` finds no per-role
    override, this returns the stock symmetric layout's slot0 window --
    right after the mcuboot region, at the MRAM base -- computed as
    `gen_zephyr_board._AEN_MRAM_BASE + gen_zephyr_board._AEN_MCUBOOT_KIB *
    1024` directly against those two imported constants, not a locally
    pinned literal. This is the SAME arithmetic
    `_aen_generate_files` performs when it hits the no-override branch
    (gen_zephyr_board.py:1319); reading its constants instead of a copy of
    their product means a change to either constant moves this default too
    -- there is nothing here for a test to keep in sync by hand. Applies
    uniformly to EVERY role in the no-override case (not just `he`): the
    stock layout has exactly one slot0 window, and whichever M55 core a
    SoM boots lands on it -- including an `m55_hp` that boots alone, the
    case a role-keyed (`he`-only) fallback would still refuse. (This
    sentence used to call E1M-AEN401/E1M-AEN601 "single-M55"; both are
    DUAL-M55 -- see the paragraph below -- and #1446 cites that wrong
    clause as the justification for having put two cores' slot0 at one
    address until #1445.)

    E1M-AEN401/E1M-AEN601 declare BOTH `m55_hp` and `m55_he` in
    `topology:` (only the HP board's Zephyr tree is generated today --
    #999, a separate, tracked gap), and since #1445 both also declare
    per-role `he_slot0`/`hp_slot0` overrides in `memory_map:`
    (metadata/e1m_modules/E1M-AEN401.yaml, E1M-AEN601.yaml), so this
    resolves to DISJOINT addresses for the two roles, not the
    no-override default above. The no-override branch is reachable only
    by a FUTURE AEN variant that publishes `jlink_flash_device` without
    also declaring a disjoint-slot0 override -- `_validate_topology_cores`
    only calls this resolver once the SoC variant publishes
    `jlink_flash_device`, and every AEN preset already declares the
    override. (Do not read that as a list of the variants which publish
    `jlink_flash_device`: E4 does not -- `metadata/socs/alif/ensemble/
    e4.json` declares `"jlink_flash_device": null`, so AEN401 never
    reaches this resolver -- while E3/E5/E7 do, for AEN301/AEN501/AEN701.) That residual risk is
    tracked, not silently accepted: #1384.
    """
    if not core_id.startswith("m55_"):
        return None
    role = core_id[len("m55_"):]
    if role not in ("he", "hp"):
        return None

    from gen_zephyr_board import (
        _AEN_MCUBOOT_KIB, _AEN_MRAM_BASE, ZephyrBoardEmitError,
        _aen_role_slot0_map)

    memory_map = som_preset.get("memory_map") or []
    try:
        slot0_region = _aen_role_slot0_map(memory_map, role)
    except ZephyrBoardEmitError as exc:
        raise OrchestratorError(
            f"cannot resolve flash_args.slot0_load_address for "
            f"{core_id!r}: {exc}") from exc

    if slot0_region is not None:
        return f"0x{slot0_region['base']:08x}"
    return f"0x{_AEN_MRAM_BASE + _AEN_MCUBOOT_KIB * 1024:08x}"


def _resolve_flow_d_preflight(
    debug: dict[str, Any],
    core_id: str,
) -> tuple[Optional[str], Optional[str]]:
    """The read-only SW-DP IDR preflight PAIR for one core, out of an
    already-resolved `debug:` block: `(expect_dpidr, jlink_device)`.

    Two facts, one guard (#1355).  `expect_dpidr` is the DPIDR the board's
    debug port must answer before a host flasher writes anything;
    `jlink_device` is the LIVE-CORE attach profile the read is performed
    with -- distinct from `jlink_flash_device`, which is the part-number
    flash-algorithm profile and cannot attach to a running core on an older
    J-Link DLL.  A consumer needs both or the check cannot run, and tan
    (`python/tan/core/flash_plan.py::validate_flow_d_preflight_args`) hard-
    refuses a half-armed pair -- `(expected is None) != (read_device is
    None)` raises, at PLAN time, for every Flow D entry including a dry run.
    So emitting one without the other does not merely leave the guard
    unarmed, it bricks the flash path outright.

    Hence both-or-neither, unconditionally: either key missing yields
    `(None, None)`.  Note `debug.jlink_device` is legitimately sparse across
    `cores[]` -- E8 publishes an attach profile for `m55_hp`/`m55_he` and
    none for `a32_cluster`, which is a Cortex-A cluster running Yocto, not a
    J-Link flash target -- so "no attach profile for THIS core" is a normal
    answer, not an error, and this function must not treat it as one.
    Whether a missing profile is instead a real metadata gap depends on the
    slice's runtime, which is only known once the Slice exists: see
    `_enforce_flow_d_preflight_pair`.

    `(None, None)` therefore means "this core does not arm the preflight",
    and is the shape for every slice except E1M-AEN801's two M55s today.
    """
    expect_dpidr = debug.get("expect_dpidr")
    jlink_device = (debug.get("jlink_device") or {}).get(core_id)
    if not expect_dpidr or not jlink_device:
        return (None, None)
    return (expect_dpidr, jlink_device)


def _enforce_flow_d_preflight_pair(
    slice_: Slice,
    debug: dict[str, Any],
    sku: str,
) -> None:
    """Refuse a Flow-D-armed Zephyr slice that silently dropped the
    wrong-board preflight because its core has no attach profile (#1355).

    `_resolve_flow_d_preflight`'s both-or-neither rule is what keeps the
    emitted `flash_args` legal, but on its own it would also let a REAL gap
    -- a variant that publishes `expect_dpidr` and forgets (or loses, to a
    core rename) the `jlink_device` entry for a core that genuinely flashes
    -- degrade quietly back to an unguarded write.  That is the exact
    failure this issue exists to remove, so it must be loud.

    Scoped to the slices where the guard actually applies: `os: zephyr`
    (Flow D is a Zephyr-on-M path) whose SoC variant publishes
    `jlink_flash_device` (per tan's `FLOW_D_KEYS`, that key's presence IS
    what promotes a `zephyr_west_flash` entry to Flow D).  An A-core, an
    `os: off` core, or a part with no J-Link MRAM loader at all is not a
    Flow D target and is correctly silent here -- checking those would fail
    every AEN project on `a32_cluster`, which is not a debug-attach target
    and never was.

    The converse -- `jlink_device` with no `expect_dpidr` -- is deliberately
    NOT an error: that is the normal, correct state of every variant whose
    DPIDR nobody has measured yet (E1M-AEN701's AE722F80F55D5LS today), and
    it simply leaves the guard unarmed.  An unarmed guard is recoverable; a
    guard armed at a guessed ID is not, because a wrong ID that happens to
    match another board on the same bench passes on exactly the board it
    exists to exclude.
    """
    if slice_.os != "zephyr" or not (
        slice_.jlink_flash_device_declared or slice_.jlink_flash_device is not None
    ):
        return
    # Bound once and read from the local: interpolating `debug['expect_dpidr']`
    # into the message would make a mis-edited condition fail with a KeyError
    # raised from inside its own diagnostic, instead of with the diagnostic.
    expect_dpidr = debug.get("expect_dpidr")
    if expect_dpidr and not slice_.jlink_device:
        raise OrchestratorError(
            f"{sku}: SoC variant publishes debug.expect_dpidr "
            f"({expect_dpidr}) but no debug.jlink_device entry for "
            f"core '{slice_.core_id}', which flashes over Flow D -- the "
            f"wrong-board SW-DP IDR preflight needs BOTH (the expected ID "
            f"and the live-core attach profile it is read with), and a "
            f"downstream flasher refuses a half-armed pair outright rather "
            f"than skipping the check. Add "
            f"debug.jlink_device['{slice_.core_id}'] to this variant in "
            f"metadata/socs/, or remove debug.expect_dpidr.")


def _enforce_slot0_disjoint_across_roles(
    cores: dict[str, Slice],
    sku: str,
) -> None:
    """Refuse a dual-M55 AEN SoM whose `m55_he` and `m55_hp` slices are BOTH
    live Zephyr Flow-D targets and publish the SAME
    `flash_args.slot0_load_address` (#1384).

    #1295 made this reachable for the first time: `_resolve_slot0_load_address`
    is only called when the SoC variant already publishes
    `debug.jlink_flash_device` (`_validate_topology_cores`, just above), and
    before #1295 only E1M-AEN801's variant did -- every other dual-M55 AEN
    SoM's no-override default (deliberately the SAME address for both roles,
    see `_resolve_slot0_load_address`'s docstring) never collided with a live
    `jlink_flash_device` in practice. #1295 populated the key for the E3/E5/
    E6/E7 variants too, which immediately surfaced this exact collision for
    E1M-AEN301 (via `examples/power-timing/power-managed-sensor/board.yaml`)
    -- except that app parks `m55_hp` with `os: "off"`, so `m55_hp` is not a
    build/flash target at all: nothing ever WOULD write to its computed slot0
    address, so a collision with a core that never gets flashed is not the
    #1069 hazard this guard exists to catch.

    Scoped to `os == "zephyr"` on BOTH roles for exactly that reason -- a
    parked (`os: "off"`) or non-Zephyr core produces no flashable artifact,
    so its resolved `slot0_load_address` is moot; comparing it anyway would
    refuse `power-managed-sensor` (a real, working, single-live-core app) for
    a collision that can never physically happen. Mirrors
    `_enforce_flow_d_preflight_pair`'s own `slice_.os != "zephyr"` guard
    immediately above, the file's established convention for "only a live
    Zephyr slice is a Flow-D target".

    Still a real guard: a future app that enables BOTH `m55_he` and `m55_hp`
    as `os: zephyr` on a SoM whose variant publishes `jlink_flash_device`
    without a disjoint-slot0 `memory_map:` override would otherwise silently
    reintroduce #1069's HE/HP MRAM collision in `flash_args` -- flashing one
    core would corrupt the other's slot0 window with no signal at all, the
    exact silent-wrong-address class this file's other guards
    (`_enforce_flow_d_preflight_pair`, `_resolve_slot0_load_address`'s own
    `OrchestratorError` on a half-authored override) all exist to remove.
    That is exactly the state AEN301/501/601/701 are in RIGHT NOW for any
    board.yaml that does NOT park the sibling core (the common case, e.g.
    `scripts/gen_portability_matrix.py`'s per-SKU sweep) -- tracked as a
    follow-up at #1445, since the fix is a per-SoM `memory_map:` decision
    (slot sizes, OTA-per-core policy) this file cannot make on its own.
    """
    he = cores.get("m55_he")
    hp = cores.get("m55_hp")
    if (he is None or hp is None
            or he.os != "zephyr" or hp.os != "zephyr"
            or he.slot0_load_address is None
            or hp.slot0_load_address is None):
        return
    if he.slot0_load_address == hp.slot0_load_address:
        raise OrchestratorError(
            f"{sku}: m55_he and m55_hp both resolve "
            f"flash_args.slot0_load_address to the same address "
            f"({he.slot0_load_address}) -- this is the #1069 HE/HP MRAM "
            f"slot0 collision (flashing one core would silently corrupt "
            f"the other's slot0 window). Declare disjoint `he_slot0`/"
            f"`hp_slot0` regions in this SoM preset's `memory_map:` (see "
            f"metadata/e1m_modules/E1M-AEN801.yaml) before this SoM can "
            f"publish debug.jlink_flash_device.")


def _slice_from_resolved(
    core_id: str,
    entry: dict[str, Any],
    soc_core_type: str = "",
    jlink_flash_device: Optional[str] = None,
    jlink_flash_device_declared: bool = False,
    expect_dpidr: Optional[str] = None,
    jlink_device: Optional[str] = None,
    slot0_load_address: Optional[str] = None,
) -> Slice:
    """Build a Slice dataclass from the resolved per-core entry.

    When `entry["os"]` is missing/empty, the OS is inferred from
    `soc_core_type` via `_default_os_from_core_type()` (cortex-m* ->
    zephyr, cortex-a* -> yocto, else "off").  Passing the empty
    string for `soc_core_type` preserves the historical default of
    "off".

    `jlink_flash_device` is the caller's already-resolved
    `_resolve_jlink_flash_device()` result (see that docstring) -- None
    when this SoC variant publishes no J-Link flash-device profile.

    `expect_dpidr` / `jlink_device` are the caller's already-resolved
    `_resolve_flow_d_preflight()` PAIR for this core -- both None (the
    preflight is not armed) or both set; never one of the two, which is
    the half-armed shape a downstream flasher refuses (#1355).

    `slot0_load_address` is the caller's already-resolved
    `_resolve_slot0_load_address()` result for THIS core_id -- None when
    this core has no AEN MRAM slot0-XIP window to publish.
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
        jlink_flash_device=jlink_flash_device,
        jlink_flash_device_declared=jlink_flash_device_declared,
        expect_dpidr=expect_dpidr,
        jlink_device=jlink_device,
        slot0_load_address=slot0_load_address,
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
    return _resolve_board_impl(project, metadata_root)


def _sku_family_dir(sku: str) -> Optional[str]:
    """The SoM-family directory name for `sku`, or None when unrecognised.

    Shared by `_check_hw_rev_exists` and `_check_sdk_supports_hw_rev`: an
    unrecognised SKU is `_resolve_board`'s error to raise, not either
    gate's to pre-empt with a worse message, so both stay quiet on
    `ValueError` rather than failing here.
    """
    try:
        from alp_project import _sku_family
        return _sku_family(sku)
    except (ImportError, ValueError):
        return None


def _check_hw_rev_exists(
    metadata_root: Path,
    *,
    sku: str,
    som_hw_rev: Optional[str],
    board_name: Optional[str],
    board_hw_rev: Optional[str],
    board_preset: Optional[dict[str, Any]],
) -> None:
    """Refuse an hw_rev that isn't a key in its resolved `hw_revisions:` table.

    An hw_rev absent from the table used to silently resolve to empty
    overrides -- base-revision pad routing with a clean exit code, i.e. a
    wrong-hardware emit (#1025, the safe half).  Runs BEFORE
    `_check_sdk_supports_hw_rev`'s min/max comparison: an unknown revision
    has no declared range to compare against, so reporting it as
    out-of-range would name the wrong cause.

    Existence-only, deliberately blind to `status:` -- a revision that
    EXISTS passes regardless of being `status: reserved`, `status: tbd`,
    or carrying no `status` key at all.  Skips a side entirely when that
    side's table doesn't exist to check against (inline boards carry no
    per-board `hw_revisions:` table).
    """
    family_dir = _sku_family_dir(sku)

    known = sdk_compat.family_revision_known(metadata_root, family_dir, som_hw_rev)
    if known is False:
        available = sdk_compat.family_available_revisions(metadata_root, family_dir)
        raise SdkRevisionUnknown(
            f"SoM {sku} hw_rev {som_hw_rev!r} is not a known hardware "
            f"revision. Available hw_rev(s) for {sku}: {available}.")

    known = sdk_compat.revision_known(board_preset, board_hw_rev)
    if known is False:
        available = sorted((board_preset or {}).get("hw_revisions", {}).keys())
        raise SdkRevisionUnknown(
            f"board {board_name} hw_rev {board_hw_rev!r} is not a known "
            f"hardware revision. Available hw_rev(s) for {board_name}: "
            f"{available}.")


def _status_repr(status: Optional[str]) -> str:
    """Render a `status:` value for an error message.

    `status: None` reads as if the key were literally set to the word
    "None" -- indistinguishable from a typo'd value.  A missing key gets
    its own wording instead.
    """
    if status is None:
        return "carries no `status:` key"
    return f"status: {status!r}"


def _check_hw_rev_buildable(
    metadata_root: Path,
    *,
    sku: str,
    som_hw_rev: Optional[str],
    board_name: Optional[str],
    board_hw_rev: Optional[str],
    board_preset: Optional[dict[str, Any]],
) -> None:
    """Refuse an hw_rev that EXISTS but whose declared `status:` refuses a
    build (#1025, the maintainer's broad-reading decision on the status
    half).

    Runs AFTER `_check_hw_rev_exists`: a revision absent from the table is
    that gate's failure to report, not this one's -- there is no status to
    have read.  `status: reserved`, `status: tbd`, and a revision carrying
    no `status` key at all are all refused; every other declared status
    (`production`, `preview`, `preliminary`, `deprecated`) passes.
    """
    family_dir = _sku_family_dir(sku)

    buildable = sdk_compat.family_revision_buildable(metadata_root, family_dir, som_hw_rev)
    if buildable is False:
        status = sdk_compat.family_revision(metadata_root, family_dir, som_hw_rev).get("status")
        raise SdkRevisionNotBuildable(
            f"SoM {sku} hw_rev {som_hw_rev!r} exists but is not buildable "
            f"({_status_repr(status)}).")

    buildable = sdk_compat.revision_buildable(board_preset, board_hw_rev)
    if buildable is False:
        status = sdk_compat.board_revision(board_preset, board_hw_rev).get("status")
        raise SdkRevisionNotBuildable(
            f"board {board_name} hw_rev {board_hw_rev!r} exists but is not "
            f"buildable ({_status_repr(status)}).")


def _check_sdk_supports_hw_rev(
    metadata_root: Path,
    *,
    sku: str,
    som_hw_rev: Optional[str],
    board_name: Optional[str],
    board_hw_rev: Optional[str],
    board_preset: Optional[dict[str, Any]],
) -> None:
    """Refuse when the running SDK is outside a requested revision's range.

    `metadata/sdk_version.yaml` has always documented this refusal -- and
    the matching `exit code 3` in `scripts/validate_board_yaml.py` -- but
    nothing implemented it, so an upgraded SDK silently emitted for a
    revision it no longer supported (#1019).

    Callers run `_check_hw_rev_exists` first: an unknown revision is that
    gate's failure, not this one's -- it has no bounds to compare.
    """
    sdk_version = sdk_compat.read_sdk_version(metadata_root)
    if sdk_version is None:
        return

    family_dir = _sku_family_dir(sku)

    reason = sdk_compat.check(
        sdk_version,
        som_revision=sdk_compat.family_revision(
            metadata_root, family_dir, som_hw_rev),
        som_label=f"SoM {sku} hw_rev {som_hw_rev}",
        board_revision_entry=sdk_compat.board_revision(
            board_preset, board_hw_rev),
        board_label=f"board {board_name} hw_rev {board_hw_rev}",
    )
    if reason:
        raise SdkRevisionUnsupported(
            f"SDK {sdk_version} does not support this hardware revision: "
            f"{reason}. Pin an SDK inside the declared range, or set a "
            f"hw_rev this SDK supports.")


def _resolve_board_impl(project: dict[str, Any],
                        metadata_root: Path) -> tuple[Any, ...]:
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

    # SoC-variant `debug:` facts, resolved from ONE variant match (see
    # `_resolve_variant_debug`).  `jlink_flash_device` is not per-core, so
    # it is resolved once and reused for every slice below; the
    # `expect_dpidr`/`jlink_device` preflight pair IS per-core (the read
    # attach profile is keyed by `cores[].id`) and is resolved inside the
    # loop.
    variant_debug = _resolve_variant_debug(som_preset, soc_spec)
    jlink_flash_device = _resolve_jlink_flash_device(variant_debug)
    jlink_flash_device_declared = _jlink_flash_device_declared(variant_debug)

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
        expect_dpidr, jlink_device = _resolve_flow_d_preflight(
            variant_debug, core_id)
        slot0_load_address = (
            _resolve_slot0_load_address(som_preset, core_id)
            if (jlink_flash_device_declared or jlink_flash_device is not None)
            else None)
        slice_ = _slice_from_resolved(
            core_id, resolved,
            soc_core_type=soc_core_type_by_id.get(core_id, ""),
            jlink_flash_device=jlink_flash_device,
            jlink_flash_device_declared=jlink_flash_device_declared,
            expect_dpidr=expect_dpidr,
            jlink_device=jlink_device,
            slot0_load_address=slot0_load_address,
        )
        _enforce_flow_d_preflight_pair(slice_, variant_debug, sku)
        _enforce_loader_rules(slice_)
        _enforce_os_matches_core_class(
            slice_, soc_core_type_by_id.get(core_id, ""))
        cores[core_id] = slice_

    _enforce_slot0_disjoint_across_roles(cores, sku)

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

    # #1088: `kind: rpmsg` has no cache-maintenance layer.  `cfg->cacheable`
    # is stored on the backend struct (src/backends/rpc/{zephyr,yocto}_drv.c)
    # and never read again -- no `sys_cache_*` call exists anywhere under
    # src/ or include/.  `cacheable: true` on a rpmsg entry would therefore
    # select a code path that promises coherency it can't deliver, which is
    # worse than no flag at all -- reject it here rather than silently
    # honouring it.  Real fix (sys_cache_data_flush_range /
    # sys_cache_data_invd_range in <alp/rpc.h>) remains open; see #1088.
    for e in ipc_entries:
        if e.kind == "rpmsg" and e.cacheable:
            raise OrchestratorError(
                f"ipc entry '{e.name}': kind: rpmsg does not support "
                f"cacheable: true -- <alp/rpc.h> has no cache-maintenance "
                f"implementation yet (#1088).  The D-cache is forced off "
                f"for this carve-out's endpoints instead; remove "
                f"`cacheable: true` (or set it to false).")

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
        # `fs` defaults to `raw` when omitted. The legacy `raw: true` alias is
        # gone: `board.schema.json` no longer declares the property and sets
        # `additionalProperties: false` on storage items, so a board carrying it
        # is rejected at validation rather than normalised here. Measured before
        # removal: zero tracked `board.yaml` files used it.
        fs = item.get("fs")
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
    doc = strict_json_loads(path.read_text(encoding="utf-8"), source=path)
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
    `cores.<id>.libraries:` list to read.  A `cores:` entry scoping a library
    to a core the topology doesn't declare is a hard error: silently dropping
    it would emit nothing for a library the app author explicitly asked for.
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
                if str(cid) not in cores_map:
                    raise OrchestratorError(
                        f"libraries: entry '{canonical}' is scoped to core "
                        f"'{cid}', which is not declared under `cores:`")
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

    _check_hw_rev_exists(
        metadata_root,
        sku=sku,
        som_hw_rev=hw_rev or som_preset.get("default_hw_rev"),
        board_name=board_name,
        board_hw_rev=board_hw_rev,
        board_preset=board_preset)

    _check_hw_rev_buildable(
        metadata_root,
        sku=sku,
        som_hw_rev=hw_rev or som_preset.get("default_hw_rev"),
        board_name=board_name,
        board_hw_rev=board_hw_rev,
        board_preset=board_preset)

    _check_sdk_supports_hw_rev(
        metadata_root,
        sku=sku,
        som_hw_rev=hw_rev or som_preset.get("default_hw_rev"),
        board_name=board_name,
        board_hw_rev=board_hw_rev,
        board_preset=board_preset)

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
