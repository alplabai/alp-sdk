#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Load + validate a board.yaml project config and emit the per-backend
native config it compiles down to.

Usage:

    # Emit a Zephyr Kconfig fragment from ./board.yaml to stdout:
    python3 scripts/alp_project.py

    # Same, explicit:
    python3 scripts/alp_project.py --input board.yaml --emit zephyr-conf

    # Plain-CMake -D args:
    python3 scripts/alp_project.py --emit cmake-args

    # Yocto local.conf snippet:
    python3 scripts/alp_project.py --emit yocto-conf

    # Per-core natural-vs-effective OS facts (JSON; for IDEs / tooling):
    python3 scripts/alp_project.py --emit os-topology

    # Write to a file (typical Zephyr usage: included by prj.conf):
    python3 scripts/alp_project.py --emit zephyr-conf \\
        --output build/generated/alp.conf

The loader resolves:
  - The SoM SKU preset under metadata/e1m_modules/<SKU>.yaml
  - The shared board definition under metadata/boards/<preset>.yaml
    (when board.yaml uses `preset:`), OR the inline top-level
    `populated:` + `e1m_routes:` block (when board.yaml defines
    its board inline)

Then emits the appropriate native config.  For Zephyr this is a
.conf file the build appends to prj.conf; for plain CMake a
sequence of `-D` args; for Yocto a local.conf snippet.

Errors are reported with a one-line summary + the underlying
schema / file path so failures are debuggable.

Dependencies (standard CPython 3.10+ stdlib + two well-established
pip packages):
  - PyYAML  (yaml parser)
  - jsonschema  (already used by validate_metadata.py)
"""

from __future__ import annotations

import argparse
import functools
import json
import re
import sys
from pathlib import Path
from typing import Any

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("alp_project: PyYAML is required.  Install via `pip install pyyaml`.")

try:
    import jsonschema  # type: ignore[import-untyped]
except ImportError:
    sys.exit("alp_project: jsonschema is required.  Install via `pip install jsonschema`.")

from alp_registries import peripheral_kconfig


REPO = Path(__file__).resolve().parent.parent
METADATA_ROOT = REPO / "metadata"
BOARD_SCHEMA = METADATA_ROOT / "schemas" / "board.schema.json"
SDK_VERSION_FILE = METADATA_ROOT / "sdk_version.yaml"


# ---------------------------------------------------------------------
# SKU-family mapping
# ---------------------------------------------------------------------
#
# The SKU-prefix -> family-dir map is a small, pure derivation with no
# second on-disk source, so we keep it inline.  When a new SoM family
# lands, add the entry here + update the schema's `som.sku` pattern.
# (The silicon -> Kconfig mapping, by contrast, now lives in the
# versioned registry below -- see silicon_to_kconfig().)

_SKU_FAMILY = re.compile(r"^E1M-(AEN|V2N|V2M|NX9)")


def _sku_family(sku: str) -> str:
    """Return the SoM family directory name for a SKU string."""
    m = _SKU_FAMILY.match(sku)
    if m is None:
        raise ValueError(f"unrecognised SoM SKU pattern: {sku}")
    return {"AEN": "aen", "V2N": "v2n", "V2M": "v2n-m1", "NX9": "imx93"}[m.group(1)]


# Silicon ref -> Zephyr SoC-select Kconfig symbol.
#
# This is NOT a hand-maintained table: the symbol is computed from the
# ref, and the single source of the allowlist is the versioned registry
# at metadata/registries/silicon-kconfig.json.  Both this emitter and
# scripts/alp_orchestrate/ consume `silicon_to_kconfig()` so the
# mapping has exactly one definition (the prior _SILICON_TO_KCONFIG dict
# was duplicated across both files -- "duplicated truth is a bug").
SILICON_KCONFIG_REGISTRY = METADATA_ROOT / "registries" / "silicon-kconfig.json"


@functools.lru_cache(maxsize=1)
def _load_silicon_kconfig() -> tuple[str, frozenset[str]]:
    """Load (socSymbolPrefix, knownSilicon) from the versioned registry."""
    data = json.loads(SILICON_KCONFIG_REGISTRY.read_text(encoding="utf-8"))
    return data["socSymbolPrefix"], frozenset(data["knownSilicon"])


def silicon_to_kconfig(silicon: str | None) -> str | None:
    """Return the Zephyr Kconfig symbol that selects *silicon*, or ``None``.

    The symbol is computed as ``socSymbolPrefix + ref.upper().replace(':','_')``;
    e.g. ``alif:ensemble:e7`` -> ``ALP_SOC_ALIF_ENSEMBLE_E7``.  ``None`` is
    returned for any ref not in the registry allowlist (so an accelerator
    such as ``deepx:dx:m1`` -- or an unknown ref -- emits no CONFIG line,
    matching the prior dict's ``.get()`` behaviour).
    """
    if silicon is None:
        return None
    prefix, known = _load_silicon_kconfig()
    if silicon not in known:
        return None
    return prefix + silicon.upper().replace(":", "_")


# ---------------------------------------------------------------------
# Load + validate
# ---------------------------------------------------------------------


def _load_yaml(path: Path) -> dict[str, Any]:
    """Delegate to the orchestrator loader's `_load_yaml` (one parse, one home).

    Same checks, same message texts -- the loader raises OrchestratorError with
    exactly the text this used to print after its "alp_project: " prefix, so the
    CLI-facing behaviour is byte-identical (sys.exit, code 1). Lazy import:
    alp_orchestrate imports this module at load time (the resolve_memory_map
    edge), so the reverse import must happen at call time.
    """
    from alp_orchestrate.loader import OrchestratorError
    from alp_orchestrate.loader import _load_yaml as _loader_load_yaml
    try:
        return _loader_load_yaml(path)
    except OrchestratorError as e:
        sys.exit(f"alp_project: {e}")


def _validate_and_load(path: Path) -> dict[str, Any]:
    """Validate *path* with the rich diagnostic validator, then return a
    plain ``dict`` for the downstream emitters.

    If the validator finds errors, each one is rendered as a Rust-style
    diagnostic block to *stderr* and the process exits with code 1.
    On success the file is parsed a second time with ``yaml.safe_load``
    so the rest of the emitter logic receives plain Python objects (no
    ``__line__``/``__column__`` noise from the position-aware loader).
    """
    try:
        from alp_cli.validator import validate_board_yaml
        from alp_cli.diagnostic import render
    except ImportError as _exc:
        # alp_cli not installed: fall back to the legacy bare loader so
        # existing workflows that haven't installed the dev extras keep
        # working; they just won't get rich diagnostics.
        import warnings
        warnings.warn(
            f"alp_project: alp_cli not importable ({_exc}); "
            "falling back to plain YAML load (no rich diagnostics).",
            stacklevel=2,
        )
        return _load_yaml(path)

    if not path.is_file():
        sys.exit(f"alp_project: file not found: {path}")

    collector = validate_board_yaml(path)
    if collector.has_errors():
        source_text = path.read_text(encoding="utf-8")
        for diag in collector:
            print(render(diag, source_text=source_text), file=sys.stderr)
        sys.exit(1)

    # Re-parse for the emitter: the position-aware loader injects
    # ``__line__`` / ``__column__`` / ``__keys__`` metadata that the
    # downstream emitters do not expect.  A plain safe_load gives them
    # clean dicts.
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as e:
        sys.exit(f"alp_project: failed to parse {path}: {e}")
    if not isinstance(data, dict):
        sys.exit(f"alp_project: {path} did not parse to a top-level mapping")
    return data


def _resolve_sku(sku: str, metadata_root: Path) -> dict[str, Any]:
    # Per-SKU preset lives at metadata/e1m_modules/<SKU>.yaml.
    preset_path = metadata_root / "e1m_modules" / f"{sku}.yaml"
    if not preset_path.is_file():
        sys.exit(
            f"alp_project: no preset for SKU {sku} at {preset_path.relative_to(REPO)} "
            f"(remaining SKUs land alongside the user-supplied HW config writeup)"
        )
    return _load_yaml(preset_path)


def _resolve_board(preset: str, metadata_root: Path) -> dict[str, Any]:
    # Shared board definition lives at metadata/boards/<preset>.yaml.
    preset_path = metadata_root / "boards" / f"{preset}.yaml"
    if not preset_path.is_file():
        sys.exit(
            f"alp_project: `preset: {preset}` does not resolve at "
            f"{preset_path.relative_to(REPO) if preset_path.is_relative_to(REPO) else preset_path}")
    return _load_yaml(preset_path)


def _resolve_inline_or_preset_board(
    project: dict[str, Any], metadata_root: Path,
) -> dict[str, Any]:
    """Return the resolved board dict for a project.

    Mirrors the orchestrator's resolution: `preset: <name>` loads
    metadata/boards/<name>.yaml; otherwise the project's own
    top-level `name`/`populated`/`e1m_routes` are wrapped into the
    same shape the downstream emitters consume.
    """
    if "preset" in project:
        return _resolve_board(project["preset"], metadata_root)
    return {
        "name":           project.get("name"),
        "populated":      dict(project.get("populated") or {}),
        "e1m_routes":     dict(project.get("e1m_routes") or {}),
        "default_hw_rev": project.get("hw_rev"),
    }


def _resolve_silicon_variant(
    sku_preset: dict[str, Any],
    metadata_root: Path,
) -> dict[str, Any] | None:
    """Resolve a SoM preset to its matching SoC-JSON variant entry.

    Two paths, in order of preference:

    1. Forward: the SoM preset's top-level `silicon_variant:` field
       names the exact `variants[].order_code` to pick out.
    2. Reverse fallback: scan the SoC JSON's `variants[]` for one
       whose `alp_module_skus` array contains the SoM SKU.

    Returns the matched variant dict (with keys order_code, package,
    mram_mb, sram_kb, optional_features, ...) or None when neither
    path resolves (the SoC JSON has no variant for this SKU AND the
    preset declares no silicon_variant, or declares `silicon_variant:
    TBD` per the no-inventing-values rule).
    """
    silicon = sku_preset.get("silicon")
    if not silicon:
        return None
    parts = silicon.split(":")
    if len(parts) != 3:
        return None
    soc_path = metadata_root / "socs" / parts[0] / parts[1] / f"{parts[2]}.json"
    if not soc_path.is_file():
        return None
    soc_spec = json.loads(soc_path.read_text(encoding="utf-8"))
    variants = soc_spec.get("variants") or []

    declared = sku_preset.get("silicon_variant")
    if declared and declared != "TBD":
        for v in variants:
            if v.get("order_code") == declared:
                return v
        # Forward declared but not found -- noisy fall-through to reverse lookup.

    sku = sku_preset.get("sku")
    if sku:
        for v in variants:
            if sku in (v.get("alp_module_skus") or []):
                return v
    return None


def _resolve_pad_routes(
    sku_preset: dict[str, Any],
) -> dict[str, dict[str, Any]]:
    """Index a SoM preset's `pad_routes:` block by E1M pad/instance.

    Returns a dict keyed by `E1M_*` identifier (e.g., `E1M_GPIO_IO15`,
    `E1M_SPI1`) with the full route entry as value:

        { "dispatch": "cc3501e", "dispatch_pin": 14, "doc": "..." }

    Pads NOT present in the preset's `pad_routes:` (or absent block
    entirely) are implicitly `direct` -- they route to the main
    silicon's GPIO / peripheral with no mediator.

    The SDK's codegen layer composes this with the board preset's
    `e1m_routes:` block: when an E1M pad appears in both, the board
    supplies the role (e.g. `bmi323_int1`) and the SoM supplies the
    dispatch path (e.g. CC3501E GPIO 14). The two blocks together
    let a customer swap SoMs (AEN801 -> NX9101) without touching the
    board YAML or any app source -- the [[som-swappable-without-board-changes]]
    promise.
    """
    routes = sku_preset.get("pad_routes") or []
    if not isinstance(routes, list):
        return {}
    indexed: dict[str, dict[str, Any]] = {}
    for entry in routes:
        if not isinstance(entry, dict):
            continue
        e1m = entry.get("e1m")
        if not isinstance(e1m, str):
            continue
        # Last-write-wins on duplicates -- the schema doesn't forbid
        # them but real SoMs shouldn't have any; if the YAML carries
        # duplicates, surface the later entry (most-recent author wins).
        indexed[e1m] = entry
    return indexed


def _hwrev_pad_route_overrides(
    sku: str,
    hw_rev: str | None,
    metadata_root: Path,
) -> list[dict[str, Any]]:
    """Per-rev pad-route overrides for the selected ``hw_rev``.

    The base SoM ``pad_routes:`` tracks the *production* revision.  A board
    revision whose routing differs declares the deviating pads as data in
    the family ``hw-revisions.yaml`` ``pad_route_overrides:`` block; this
    returns that rev's list (empty when it declares none, so the base
    ``pad_routes:`` then applies verbatim).  Applying these is what makes
    ``--emit composed-route-table`` differ between revisions of one SKU --
    e.g. AEN ``r1`` restores IO8/IO10 to Alif GPIOs and IO21 to the CC3501E,
    the pre-2626-R2 routing.
    """
    if not hw_rev:
        return []
    try:
        family = _sku_family(sku)
    except ValueError:
        return []
    path = metadata_root / "e1m_modules" / family / "hw-revisions.yaml"
    if not path.is_file():
        return []
    data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    rev = (data.get("hw_revisions") or {}).get(hw_rev) or {}
    overrides = rev.get("pad_route_overrides") or []
    return [e for e in overrides
            if isinstance(e, dict) and isinstance(e.get("e1m"), str)]


def _compose_route(
    e1m_pad: str,
    board_route: dict[str, Any] | None,
    pad_routes: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    """Compose a board `e1m_routes:` entry (role on the board
    side) with the SoM's `pad_routes:` entry (dispatch path on the
    SoM side) for a single E1M pad / instance.

    Returns a dict with: `board_role`, `board_macro`, `dispatch`
    (defaults to `direct` when the SoM declares no proxy),
    `dispatch_pin`, plus any docs.

    Callers use the composed dict to drive codegen (Zephyr DTS
    overlays, dispatch shims, capability validation).
    """
    out: dict[str, Any] = {"e1m": e1m_pad}
    if board_route:
        out["board_role"] = board_route.get("role")
        out["board_macro"] = board_route.get("macro")
        if board_route.get("doc"):
            out["board_doc"] = board_route["doc"]
    som_route = pad_routes.get(e1m_pad)
    if som_route:
        out["dispatch"] = som_route.get("dispatch", "direct")
        if som_route.get("dispatch_pin") is not None:
            out["dispatch_pin"] = som_route["dispatch_pin"]
        if som_route.get("doc"):
            out["som_doc"] = som_route["doc"]
    else:
        out["dispatch"] = "direct"
    return out


def resolve_memory_map(
    sku_preset: dict[str, Any],
    metadata_root: Path,
) -> list[dict[str, Any]]:
    """Derive the effective memory-region table for a SoM.

    Precedence:
      1. If the SoM preset declares `memory_map:` explicitly (used
         only for non-stock partitioning -- e.g. reserving SRAM for
         a hardware secure enclave outside the default layout), it
         wins verbatim.
      2. Otherwise the loader derives the region list from the SoC
         JSON variant resolved via `silicon_variant:`. Each named
         SRAM bank becomes one region; the MRAM becomes one region.
         Per-core TCM banks (SRAM names ending `_<core>_(ITCM|DTCM)`)
         carry `accessible_from: [<core>]`; un-suffixed banks are
         shared across every core declared in the variant's parent
         SoC `cores[]` list.

    The returned dicts have the keys defined by the memory_region
    schema: `name`, `size_kib`, `accessible_from`, `cacheable` (plus
    optional `base` only when the SoM preset's override declares one
    -- silicon-default bases stay unset, so downstream emitters know
    to use the silicon's defaults).

    Returns an empty list when the silicon_variant cannot be resolved
    (e.g. NX9101's `silicon_variant: TBD`) -- callers should treat
    that as "memory layout pending the HW-config writeup".
    """
    declared = sku_preset.get("memory_map")
    if declared:
        # SoM-side override wins; the loader trusts the maintainer.
        return list(declared)

    variant = _resolve_silicon_variant(sku_preset, metadata_root)
    if not variant:
        return []

    # Re-load the SoC JSON to grab the cores[] topology (the variant
    # dict alone doesn't carry per-core ids).
    silicon = sku_preset.get("silicon", "")
    parts = silicon.split(":")
    if len(parts) != 3:
        return []
    soc_path = metadata_root / "socs" / parts[0] / parts[1] / f"{parts[2]}.json"
    if not soc_path.is_file():
        return []
    soc_spec = json.loads(soc_path.read_text(encoding="utf-8"))
    soc_cores = [c.get("id") for c in soc_spec.get("cores", []) if c.get("id")]

    regions: list[dict[str, Any]] = []

    # Prefer SoC-level fixed memory_regions when present — these carry
    # authoritative base addresses (e.g. RZ/V2N OCRAM at 0x00010000).
    soc_memory_regions = soc_spec.get("memory_regions")
    if soc_memory_regions:
        return list(soc_memory_regions)

    # MRAM as one region (size in KiB; mram_mb -> *1024).
    mram_mb = variant.get("mram_mb")
    if isinstance(mram_mb, (int, float)) and mram_mb > 0:
        regions.append({
            "name": "mram_main",
            "size_kib": int(mram_mb * 1024),
            "accessible_from": list(soc_cores),
            "cacheable": True,
        })

    # SRAM banks. Per-core TCM banks get `accessible_from: [<core>]`;
    # the rest are shared across every core in the SoC.
    sram_banks = variant.get("sram_banks_kb") or {}
    for bank_name, size_kib in sram_banks.items():
        if not isinstance(size_kib, int) or size_kib <= 0:
            continue
        # Detect per-core TCM by suffix.
        accessible: list[str] = list(soc_cores)
        for core_id in soc_cores:
            suffix_token = f"_{core_id.upper()}_"
            if suffix_token in bank_name.upper():
                accessible = [core_id]
                break
        # TCMs are typically non-cacheable; bulk SRAM is cacheable.
        is_tcm = "ITCM" in bank_name.upper() or "DTCM" in bank_name.upper()
        regions.append({
            "name": bank_name.lower(),
            "size_kib": int(size_kib),
            "accessible_from": accessible,
            "cacheable": not is_tcm,
        })

    return regions


def resolve_capabilities(
    sku_preset: dict[str, Any],
    metadata_root: Path,
) -> dict[str, Any]:
    """Compose silicon capabilities from the SoC JSON with SoM-side overrides/extensions.

    Returns a dict where SoM-declared keys override SoC-declared defaults for the same key
    (e.g., V2N's `cau: true` via the GD32 bridge overrides the RZ/V2N silicon's `cau: false`).
    Keys that exist on only one side are passed through unchanged.

    Precedence:
      1. Resolve the SoC ref via the ``silicon:`` field (same path used by
         ``_resolve_silicon_variant`` and ``resolve_memory_map``).
      2. Read ``soc["capabilities"]`` (defaults to ``{}`` when absent --
         older SoC JSONs that pre-date this field continue to work).
      3. Read ``sku_preset.get("capabilities", {})``.
      4. Merge; SoM-side wins on key collision so that SoM add-on chips /
         GD32 bridge capabilities can override the host silicon's defaults.
      5. Apply the SKU's ``silicon_capabilities.unpopulated`` RESTRICTION
         list: each listed silicon capability is forced to 0 (count) /
         ``False`` (flag).  A SKU can only remove what the silicon offers
         (enforced by scripts/validate_metadata.py); presets without the
         field keep the full silicon capability set.
    """
    silicon = sku_preset.get("silicon", "")
    parts = silicon.split(":")
    soc_caps: dict[str, Any] = {}
    if len(parts) == 3:
        soc_path = metadata_root / "socs" / parts[0] / parts[1] / f"{parts[2]}.json"
        if soc_path.is_file():
            soc_spec = json.loads(soc_path.read_text(encoding="utf-8"))
            soc_caps = soc_spec.get("capabilities") or {}

    som_caps: dict[str, Any] = sku_preset.get("capabilities") or {}

    # SoM side wins on collision (bridge / add-on overrides silicon default).
    merged: dict[str, Any] = {**soc_caps, **som_caps}

    # SKU-level restriction: capabilities the silicon offers but this SKU
    # leaves unpopulated (per-SKU granularity -- one family, many SKUs).
    # Preserve the value class so count-style caps (ethos_u55_count, ...)
    # restrict to 0 and flag-style caps restrict to False.
    for name in som_unpopulated_capabilities(sku_preset):
        base = soc_caps.get(name)
        if isinstance(base, int) and not isinstance(base, bool):
            merged[name] = 0
        else:
            merged[name] = False
    return merged


def som_unpopulated_capabilities(sku_preset: dict[str, Any]) -> list[str]:
    """Return the SKU's `silicon_capabilities.unpopulated` list (or []).

    Single accessor for the per-SKU capability RESTRICTION block so the
    loader (`resolve_capabilities`), the emitters (`alp_orchestrate/kconfig.py`,
    which passes `-DALP_SOM_<TOKEN>` only for restricted SKUs) and the header
    generator (`gen_soc_caps.py`) agree on where the field lives.
    """
    block = sku_preset.get("silicon_capabilities") or {}
    if not isinstance(block, dict):
        return []
    names = block.get("unpopulated") or []
    if not isinstance(names, list):
        return []
    return [str(n) for n in names]


# ---------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------


# §D.lib.loader: map _sku_family() return values to the soc_family
# tokens used in metadata/library-profiles/<name>/hw-backends.yaml.
_SOC_FAMILY_TOKEN: dict[str, str] = {
    "aen":    "alif_ensemble",
    "v2n":    "renesas_rzv2n",
    "v2n-m1": "renesas_rzv2n",     # DEEPX add-on; HW-acc tokens still resolve via host family.
    "imx93":  "nxp_imx9",
}


def _emit_library_hw_backends(libs: list[str], sku: str) -> list[str]:
    """Per-library HW-accelerator binding loader.

    For each enabled library that ships a
    `metadata/library-profiles/<name>/hw-backends.yaml`, pick the
    highest-priority matching backend per accelerator class given the
    active SoM SKU and emit the matching `CONFIG_*=y` line.

    Match rules (per priority entry; checked in order, all specified
    keys must match):
      - `silicon:` key, if present, must equal the SKU's `silicon:`
        ref exactly (e.g. `alif:ensemble:e4`).  Lets us pin a backend
        to specific SoCs in a family -- Ethos-U85 on E4/E6/E8, but
        not on E3/E5/E7 which carry only U55.
      - `soc_family:` key, if present, must equal the SKU family
        token (`alif_ensemble`, `renesas_rzv2n`, ...).  Selects every
        SKU in that family.
      - `requires_cap:` key, if present, must name a capability flag
        in the SKU's `metadata/e1m_modules/<sku>.yaml` `capabilities:`
        block that resolves to a truthy value (`true` or a non-zero
        count).  Cleanest matcher when an accelerator is shared
        across families (e.g. `optiga_trust_m` is populated on AEN
        + V2N).
      - All three omitted = universal entry (e.g. plain FPU, generic
        DMA), matches any SKU.
    """
    import re
    from pathlib import Path

    family       = _sku_family(sku)
    soc_token    = _SOC_FAMILY_TOKEN.get(family)
    if soc_token is None:
        return []

    out: list[str] = []
    repo_root = Path(__file__).resolve().parent.parent

    # Resolve the SKU's `silicon:` ref and merged capabilities (SoC JSON
    # defaults + SoM-level overrides) via resolve_capabilities().  This
    # replaces the former inline YAML text-parser so that silicon-determined
    # capabilities removed from SoM YAMLs (Task 3, slice 3b) continue to
    # resolve from the SoC JSON that sibling Agent D populated.
    silicon_ref: str | None = None
    sku_path = repo_root / "metadata" / "e1m_modules" / f"{sku}.yaml"
    sku_preset: dict[str, Any] = {}
    if sku_path.exists():
        sku_preset = _load_yaml(sku_path) or {}
        silicon_ref = sku_preset.get("silicon")

    merged_caps: dict[str, Any] = resolve_capabilities(sku_preset, repo_root / "metadata")

    def _cap_truthy(name: str) -> bool:
        v = merged_caps.get(name)
        if v is None:
            return False
        if isinstance(v, bool):
            return v
        if isinstance(v, int):
            return v > 0
        # String value from a YAML-loaded dict (should not occur after
        # resolve_capabilities, but guard for safety).
        sv = str(v).lower()
        if sv in ("true", "yes"):
            return True
        if sv in ("false", "no", "null", "none", "0"):
            return False
        try:
            return int(sv) > 0
        except ValueError:
            return False

    for lib in libs:
        prof = repo_root / "metadata" / "library-profiles" / lib / "hw-backends.yaml"
        if not prof.exists():
            continue
        try:
            text = prof.read_text(encoding="utf-8")
        except OSError:
            continue

        # Cheap line-driven parse: every `      - { ... kconfig: CONFIG_X=y }`
        # entry sits inside a `priority:` block; we walk top-down to keep
        # the per-class first-match.  No yaml dependency on the loader.
        per_class_emitted: set[str] = set()
        current_class: str | None   = None
        for raw in text.splitlines():
            cls_match = re.match(r"^\s*-\s*class:\s*(\S+)", raw)
            if cls_match:
                current_class = cls_match.group(1)
                continue
            if current_class is None or current_class in per_class_emitted:
                continue
            entry = re.match(r"^\s*-\s*\{\s*(.+)\s*\}\s*$", raw)
            if not entry:
                continue
            kv: dict[str, str] = {}
            for tok in entry.group(1).split(","):
                tok = tok.strip()
                if not tok or ":" not in tok:
                    continue
                k, v = tok.split(":", 1)
                kv[k.strip()] = v.strip()
            sili = kv.get("silicon")
            sf   = kv.get("soc_family")
            cap  = kv.get("requires_cap")
            kcv  = kv.get("kconfig")
            if not kcv:
                continue
            # All specified matchers must succeed.
            if sili is not None and sili != silicon_ref:
                continue
            if sf is not None and sf != soc_token:
                continue
            if cap is not None and not _cap_truthy(cap):
                continue
            out.append(f"{kcv}  # {lib} / {current_class}")
            per_class_emitted.add(current_class)

    return out


# Chip name -> Zephyr subsystem CONFIG_* keys the chip driver
# depends on.  Mirrors the `depends on ...` line in each
# `config ALP_SDK_CHIP_<NAME>` entry in zephyr/Kconfig: enabling
# a chip driver doesn't auto-select its subsystem, so the loader
# emits the matching `CONFIG_<SUBSYS>=y` here.
_CHIP_SUBSYSTEMS: dict[str, tuple[str, ...]] = {
    # GPIO-only
    "button_led":         ("GPIO",),
    "cam_mux_pi3wvr626":  ("GPIO",),
    # SPI + GPIO
    "ssd1331":            ("SPI", "GPIO"),
    "cc3501e":            ("SPI", "GPIO"),
    # I2C + GPIO
    "tas2563":            ("I2C", "GPIO"),
    # I2C-only
    "lsm6dso":            ("I2C",),
    "ssd1306":            ("I2C",),
    "bme280":             ("I2C",),
    "lis2dw12":           ("I2C",),
    "ov5640":             ("I2C",),
    "icm42670":           ("I2C",),
    "bmi323":             ("I2C",),
    "bmp581":             ("I2C",),
    "tmp112":             ("I2C",),
    "rv3028c7":           ("I2C",),
    "optiga_trust_m":     ("I2C",),
    "eeprom_24c128":      ("I2C",),
    "tcal9538":           ("I2C",),
    "ina236":             ("I2C",),
    # pdm_mic helper has no subsystem dep declared in Kconfig
    # (uses <alp/i2s.h> when enabled at v0.2+).
    # v0.5 §D.AI batch -- 18 vision / display / accelerator chips.
    "ov2640":             ("I2C",),
    "ov5645":             ("I2C",),
    "ov7670":             ("I2C",),
    "ov9281":             ("I2C",),
    "ar0234":             ("I2C",),
    "imx219":             ("I2C",),
    "imx477":             ("I2C",),
    "gc2145":             ("I2C",),
    "ti_ds90ub953_954":   ("I2C",),
    "maxim_max9295_9296": ("I2C",),
    "st7789":             ("SPI", "GPIO"),
    "ili9341":            ("SPI", "GPIO"),
    "ili9488":            ("SPI", "GPIO"),
    "ra8875":             ("SPI",),
    "sh1106":             ("I2C",),
    "il3820":             ("SPI", "GPIO"),
    "gdew0154t8":         ("SPI", "GPIO"),
    "hailo_8l":           ("GPIO",),
    # v0.5 §D.industrial batch -- 18 industrial sensing / control chips.
    "bmp390":             ("I2C",),
    "ms5611":             ("I2C",),
    "lps22hb":            ("I2C",),
    "vl53l1x":            ("I2C",),
    "vl53l5cx":           ("I2C",),
    "a02yyuw":            ("SERIAL",),
    "drv8833":            ("PWM",),
    "drv8825":            ("PWM", "GPIO"),
    "tmc2209":            ("SERIAL",),
    "a4988":              ("PWM", "GPIO"),
    "as5048a_b":          ("I2C",),
    "mt6701":             ("I2C",),
    "hx711":              ("GPIO",),
    "max31855":           ("SPI",),
    "max31865":           ("SPI",),
    "tsl2591":            ("I2C",),
    "qmc5883l":           ("I2C",),
    "veml7700":           ("I2C",),
    # v0.5 §D.iot batch -- 9 IoT / connectivity chips.
    "quectel_bg95":       ("SERIAL",),
    "quectel_bg77":       ("SERIAL",),
    "ublox_sara_r5":      ("SERIAL",),
    "semtech_sx1262":     ("SPI", "GPIO"),
    "semtech_sx1276":     ("SPI", "GPIO"),
    "ublox_neo_m9n":      ("SERIAL",),
    "ublox_max_m10s":     ("SERIAL",),
    "atgm336h":           ("SERIAL",),
    "atecc608b":          ("I2C",),
    # v0.5 §D.audio batch -- 6 audio chips.
    "ics_43434":          (),                 # no Zephyr subsystem dep; sample flow via <alp/i2s.h>
    "inmp441":            (),
    "wm8960":             ("I2C",),
    "tlv320aic3204":      ("I2C",),
    "max98357a":          ("GPIO",),
    "es8388":             ("I2C",),
}


# Peripheral name (from board.yaml's `peripherals:` array) -> Zephyr Kconfig
# symbol.  Single-sourced in metadata/registries/peripheral-kconfig.json and
# shared with alp_orchestrate/slugs.py.
_PERIPHERAL_KCONFIG: dict[str, str] = peripheral_kconfig()


# Library-name -> Kconfig flag(s) to set when the library appears
# in board.yaml's `libraries:` array.  Only USER-facing libraries are
# listed here -- SDK-internal libs (LwRB for audio DMA staging,
# nanopb for mproc IPC framing) are pulled in unconditionally by
# their consumer code, no enable flag needed.
_LIBRARY_KCONFIG: dict[str, tuple[str, ...]] = {
    # User-facing C++ libs (Tier 1) -- header-only, no Kconfig
    # in Zephyr; the loader just adds the profile dir to the
    # include path via a v0.4 CMake hook.  The TODO comment
    # surfaces in the emitted alp.conf so consumers can see
    # what's pending.
    "etl":           ("# etl: include path + etl_profile.h via the v0.4 loader hook",),
    "fmt":           ("# fmt: include path + fmt_config.h via the v0.4 loader hook",),
    "nlohmann_json": ("# nlohmann_json: include path + json_config.h via the v0.4 loader hook",),
    "doctest":       ("# doctest: include path + doctest_config.h via the v0.4 loader hook",),
    # Zephyr-native libs (Tier 3) -- the SDK forwards the
    # consumer's intent to Zephyr's own Kconfig + adds the profile
    # header to the include path.
    # Baseline Tier 3 libs: emit the upstream Zephyr Kconfig + the
    # matching ALP-side SW-fallback knob.  The SW-fallback line is
    # redundant with Kconfig.alp-libraries' `default y`, but emitting
    # it explicitly in alp.conf documents the fallback choice next
    # to the library-enable line.
    "lvgl":          ("CONFIG_LVGL=y",
                      "CONFIG_ALP_LVGL_SW_BLIT=y"),
    "mbedtls":       ("CONFIG_MBEDTLS=y", "CONFIG_MBEDTLS_BUILTIN=y",
                      "CONFIG_ALP_MBEDTLS_PURE_C=y"),
    "cmsis_dsp":     ("CONFIG_CMSIS_DSP=y",
                      # CMSIS-DSP's per-component switches are off by default in
                      # the upstream Zephyr module -- enabling CMSIS_DSP alone
                      # only pulls in BASICMATH.  We turn on every component
                      # consumers might reach so kernels like arm_rfft_fast_*
                      # (TRANSFORM), arm_biquad_cascade_* (FILTERING),
                      # arm_correlate_* (STATISTICS), etc. link cleanly.  Cost
                      # is minimal -- LD's --gc-sections drops unused symbols.
                      "CONFIG_CMSIS_DSP_BASICMATH=y",
                      "CONFIG_CMSIS_DSP_COMPLEXMATH=y",
                      "CONFIG_CMSIS_DSP_CONTROLLER=y",
                      "CONFIG_CMSIS_DSP_FASTMATH=y",
                      "CONFIG_CMSIS_DSP_FILTERING=y",
                      "CONFIG_CMSIS_DSP_INTERPOLATION=y",
                      "CONFIG_CMSIS_DSP_MATRIX=y",
                      "CONFIG_CMSIS_DSP_STATISTICS=y",
                      "CONFIG_CMSIS_DSP_SUPPORT=y",
                      "CONFIG_CMSIS_DSP_TRANSFORM=y",
                      "CONFIG_ALP_CMSIS_DSP_SCALAR=y"),
    "littlefs":      ("CONFIG_FILE_SYSTEM_LITTLEFS=y", "CONFIG_FILE_SYSTEM=y",
                      "CONFIG_ALP_LITTLEFS_SYNC_IO=y"),

    # v0.5 §D.lib batch -- 17 new libraries.  Per-library hardware-
    # accelerator binding is declared in
    # metadata/library-profiles/<name>/hw-backends.yaml and emitted
    # by the §D.lib.loader hook (next commit).  The entries here just
    # surface the per-library include-path + base-Kconfig hook so the
    # consumer's `libraries: [...]` enumeration works end-to-end.
    # SW-fallback CONFIG_* knobs are emitted unconditionally; the
    # HW-backend CONFIG_* knobs come from the cross-reference loader.

    # §D.lib.ai
    "tflite_micro":   ("CONFIG_ALP_TFLM_REF_KERNELS=y",
                       "# tflite_micro: include path + tflm_config.h via v0.4 loader hook",),
    "u8g2":           ("CONFIG_ALP_U8G2_SW_BLIT=y",
                       "# u8g2: include path + u8g2_config.h via v0.4 loader hook",),
    "gfx_compat":     ("CONFIG_ALP_GFX_COMPAT_SW=y",
                       "# gfx_compat: maintainer-shipped thin shim; no external dep",),

    # §D.lib.industrial
    "madgwick_ahrs":  ("CONFIG_ALP_MADGWICK_LIBM=y",),
    "pid":            ("CONFIG_ALP_PID_INT_MATH=y",),
    "modbus":         ("CONFIG_ALP_MODBUS_SYNC_IO=y",),

    # §D.lib.iot
    "coremqtt_sn":    ("CONFIG_ALP_MQTTSN_NO_TLS=y",),
    "libcoap":        ("CONFIG_ALP_COAP_NO_TLS=y",),
    "tinygsm":        ("CONFIG_ALP_TINYGSM_SYNC_IO=y",),
    "nanopb":         ("CONFIG_ALP_NANOPB_SW=y",),
    "libwebsockets":  ("CONFIG_ALP_LWS_NO_TLS=y",),
    "jsmn":           ("CONFIG_ALP_JSMN_SW=y",),
    "bearssl":        ("CONFIG_ALP_BEARSSL_PURE_C=y",),

    # §D.lib.audio
    "minimp3":        ("CONFIG_ALP_MINIMP3_PURE_C=y",),
    "opus":           ("CONFIG_ALP_OPUS_PURE_C=y",),
    "libhelix":       ("CONFIG_ALP_LIBHELIX_PURE_C=y",),

    # §D.lib.test
    "catch2":         ("CONFIG_ALP_CATCH2_SW=y",),
}


# ---------------------------------------------------------------------
# DTS overlay emission (v0.3: i2c / spi / uart / pwm / gpio aliases)
# ---------------------------------------------------------------------
#
# Per the project memory note "pending exact hardware configurations
# -- mark unknowns TBD, never invent values", the loader translates
# the macros in include/alp/boards/<board>.h verbatim; it does not
# invent gpio bank numbers or per-pad GPIO_ACTIVE_* flags.  The
# emitted .overlay declares the board's bus aliases and a stub
# alp,pin-array with one entry per EVK_PIN_* macro, each annotated
# with a comment naming the macro and the ALP_E1M_GPIO_IO<N> it
# resolves to.  Customers fill the gpio bank / index columns with
# their SoM's actual DT controller phandles once the upstream board
# files land in alplabai/alp-zephyr-modules.
#
# Bus phandle naming convention matches the manually-written EVK
# overlays at tests/zephyr/peripheral/boards/alp_e1m_evk_aen.overlay:
# &i2c<N>, &spi<N>, &uart<N>, &pwm<N>.  Per-SoC vendor DT may use
# alternate names (e.g. &lpi2c0 on some Alif boards); the customer
# fixes the phandle if their board file diverges -- the loader's
# job is to surface every alias the board wants, not to second-
# guess vendor DT naming.

# Match `#define <NAME> ALP_E1M_<CLASS><N>` (with optional trailing
# token).  Class is one of the bus / pwm / gpio / analog-converter
# names we care about.  ADC + DAC join the set so the portable
# <alp/adc.h> / <alp/dac.h> backends -- which resolve their channels
# via the `alp-adcN` / `alp-dacN` DT aliases -- get a generated alias
# scaffold from the board's `e1m_routes.adc` / `.dac` entries.
_DEFINE_E1M_RE = re.compile(
    r"^\s*#\s*define\s+(\w+)\s+ALP_E1M_(I2C|SPI|UART|PWM|ADC|DAC|GPIO_IO)(\d+)\b",
    re.MULTILINE,
)

# Bus-alias buckets the loader emits.  Each entry maps the e1m_pinout
# class name -> (alias prefix, Zephyr DT phandle prefix).
#
# The phandle prefix is the convention-default node-label (&i2c0,
# &adc0, ...); vendor DT may use a different label (e.g. the Alif
# Ensemble ADCs are node-labelled `adc12_0` and the EEPROM bus is
# `i2c2`), in which case the per-app board overlay repoints the alias
# (`aliases { alp-adc0 = &adc12_0; };`) -- the loader's job is to
# surface every alias the board wires, not to second-guess vendor DT
# node-label naming.
_BUS_BUCKETS: tuple[tuple[str, str, str], ...] = (
    ("I2C",  "alp-i2c",  "i2c"),
    ("SPI",  "alp-spi",  "spi"),
    ("UART", "alp-uart", "uart"),
    ("PWM",  "alp-pwm",  "pwm"),
    ("ADC",  "alp-adc",  "adc"),
    ("DAC",  "alp-dac",  "dac"),
)


# Canonical E1M GPIO index order -- e1m_pinout.h "Devicetree / overlay
# invariant".  The alp,pin-array `gpios` property MUST list these 52
# entries in this exact order so the GPIO backend's positional resolve
# (alp_z_gpio_resolve -> alp_pins[pin_id]) lands on the right pad,
# including secondary-function pads opened as GPIO via ALP_E1M_GPIO_<class><N>
# (PWM -> 26..33, ENC -> 34..41, ADC -> 42..49, DAC -> 50..51).
def _e1m_gpio_canonical() -> list[str]:
    """Return the 52 ALP_E1M_GPIO_<suffix> names in canonical index order."""
    names: list[str] = [f"IO{n}" for n in range(26)]      # 0..25
    names += [f"PWM{n}" for n in range(8)]                 # 26..33
    for e in range(4):                                     # 34..41
        names += [f"ENC{e}_X", f"ENC{e}_Y"]
    names += [f"ADC{n}" for n in range(8)]                 # 42..49
    names += ["DAC0", "DAC1"]                              # 50..51
    return names


def _strip_c_comments(text: str) -> str:
    """Strip /* ... */ and // ... comments from C source text."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def _collapse_line_continuations(text: str) -> str:
    """Join `\\<newline>` continuations into single logical lines so a
    multi-line `#define NAME \\\n    VALUE` shows up as one line."""
    return re.sub(r"\\\s*\n\s*", " ", text)


def _board_header_path(board_name: str, repo_root: Path) -> Path:
    """Resolve include/alp/boards/alp_<board>.h for a board name.

    Example: 'E1M-EVK' -> include/alp/boards/alp_e1m_evk.h.
    """
    fname = "alp_" + board_name.lower().replace("-", "_") + ".h"
    return repo_root / "include" / "alp" / "boards" / fname


_INCLUDE_LOCAL_RE = re.compile(
    r'^\s*#\s*include\s+"(alp/boards/[^"]+\.h)"', re.MULTILINE,
)


def _read_board_header_with_includes(header_path: Path) -> str:
    """Read `header_path` and inline any `#include "alp/boards/<file>.h"`
    that exists under include/.  Used so the loader picks up the
    generated routes header (alp_e1m_evk_routes.h) which holds the
    EVK_* -> ALP_E1M_* macro bindings since slice 1c.

    Single-level inlining is sufficient -- the generated routes header
    only `#include`s `alp/e1m_pinout.h`, which carries no EVK_* macros.
    """
    text = header_path.read_text(encoding="utf-8")
    include_root = header_path.parent.parent.parent  # .../include/
    pieces: list[str] = [text]
    for m in _INCLUDE_LOCAL_RE.finditer(text):
        inc_rel = m.group(1)
        inc_path = include_root / inc_rel
        if inc_path.is_file() and inc_path.resolve() != header_path.resolve():
            pieces.append(inc_path.read_text(encoding="utf-8"))
    return "\n".join(pieces)


def _parse_board_macros(
    header_path: Path,
) -> dict[str, list[tuple[str, int]]]:
    """Return {class_name: [(macro_name, channel_index), ...]} for
    each ALP_E1M_<CLASS><N> reference in the board header."""
    raw = _read_board_header_with_includes(header_path)
    text = _strip_c_comments(_collapse_line_continuations(raw))
    out: dict[str, list[tuple[str, int]]] = {
        "I2C": [], "SPI": [], "UART": [], "PWM": [], "GPIO_IO": [],
    }
    for m in _DEFINE_E1M_RE.finditer(text):
        macro_name = m.group(1)
        cls = m.group(2)
        idx = int(m.group(3))
        out.setdefault(cls, []).append((macro_name, idx))
    return out


# ---------------------------------------------------------------------
# Carrier peripheral DT-wiring catalog (single source of truth)
# ---------------------------------------------------------------------
#
# Declaring a peripheral in board.yaml (`cores.<id>.peripherals`) sets the
# subsystem CONFIG via the conf emit -- but that alone does NOT bind hardware:
# the controller node sits `disabled` in the SoC dtsi, so e.g. ADC_ALIF never
# selects and `alp adc read` returns -ENOENT.  Examples used to paper over this
# with a hand-written boards/*.overlay enabling the node + the alp-<x>N alias /
# io-channels consumer the portable backends resolve.
#
# This catalog moves that wiring into the codegen: `_emit_dts_overlay` renders
# the fragment for each declared peripheral, so `peripherals: [adc]` ALONE
# yields a working `alp adc read` with NO per-example overlay.  Keyed by SoM
# family (from `_sku_family`); each entry carries the dt-bindings `#include`s it
# needs + a self-contained DTS fragment (DT permits repeated `/{}` and
# `&label{}` sections).  To add or fix a peripheral's wiring, edit ONE entry
# here -- never per example.  A per-example boards/*.overlay still layers last
# and wins (override tier), so this is opt-in-complete, not a straitjacket.
_PERIPH_DT_WIRING: dict[str, dict[str, dict[str, Any]]] = {
    "aen": {
        "i2c": {
            "include": ["zephyr/dt-bindings/i2c/i2c.h",
                        "zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h"],
            "dts": (
                "&pinctrl {\n"
                "\tpinctrl_i2c2: pinctrl_i2c2 {\n"
                "\t\tgroup0 {\n"
                "\t\t\tpinmux = <PIN_P5_6__I2C2_SCL_C>, <PIN_P5_7__I2C2_SDA_C>;\n"
                "\t\t\tinput-enable;\n"
                "\t\t\tbias-pull-down;\n"
                "\t\t};\n"
                "\t};\n"
                "};\n"
                "&i2c2 {\n"
                "\tstatus = \"okay\";\n"
                "\tpinctrl-0 = <&pinctrl_i2c2>;\n"
                "\tpinctrl-names = \"default\";\n"
                "\tclock-frequency = <I2C_BITRATE_STANDARD>;\n"
                "};\n"
                "/ {\n"
                "\taliases {\n"
                "\t\talp-i2c0 = &i2c2;\n"
                "\t};\n"
                "};\n"
            ),
        },
        "gpio": {
            "include": [],
            "dts": "&gpio8 {\n\tstatus = \"okay\";\n};\n",
        },
        "adc": {
            "include": ["zephyr/dt-bindings/adc/adc.h"],
            "dts": (
                "/ {\n"
                "\t/* alp-adc0 -> io-channels consumer; ADC_DT_SPEC_GET reads\n"
                "\t * .dev from the controller + .channel from input cell 0. */\n"
                "\talp_adc_in0: alp-adc-in0 {\n"
                "\t\tcompatible = \"alp,adc-input\";\n"
                "\t\tio-channels = <&adc12_0 0>;\n"
                "\t};\n"
                "\taliases {\n"
                "\t\talp-adc0 = &alp_adc_in0;\n"
                "\t};\n"
                "};\n"
                "&adc12_0 {\n"
                "\tstatus = \"okay\";\n"
                "\tchannel@0 {\n"
                "\t\treg = <0>;\n"
                "\t\tzephyr,gain = \"ADC_GAIN_1\";\n"
                "\t\tzephyr,reference = \"ADC_REF_INTERNAL\";\n"
                "\t\tzephyr,vref-mv = <1800>;\n"
                "\t\tzephyr,acquisition-time = <ADC_ACQ_TIME_DEFAULT>;\n"
                "\t\tzephyr,resolution = <12>;\n"
                "\t};\n"
                "};\n"
            ),
        },
    },
}


def _emit_dts_overlay(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    board_preset: dict[str, Any] | None,
    *,
    v2_peripherals: list[str] | None = None,
    v2_core_id: str | None = None,
    v2_core_os: str | None = None,
) -> str:
    """Emit a Zephyr DTS overlay describing the board wiring.

    v1 path (`v2_peripherals is None`): reads project-level
    `peripherals:` implicitly via the board header macros.

    v2 path: the project's peripherals live under `cores.<id>.peripherals`.
    Callers compute the union across Zephyr/baremetal cores (or pick one
    when `--core <id>` is supplied) and pass it in via `v2_peripherals`.
    The list is currently informational -- the bus aliases + `alp,pin-array`
    binding root node are derived from the board header, which describes
    the SoM mounting, not the project.  When `v2_core_os` is set to a
    non-Zephyr runtime (`yocto`, `off`, ...), the emitter returns a stub
    overlay with just the header comment.
    """
    lines: list[str] = []
    lines.append("/*")
    lines.append(" * Auto-generated by scripts/alp_project.py "
                 "-- do not edit by hand.")
    lines.append(" * Regenerate after changes to board.yaml or "
                 "include/alp/boards/<board>.h.")
    lines.append(" *")
    lines.append(" * Per-pad GPIO bank/index values are TBD pending the upstream")
    lines.append(" * alp_<board>_<som>.dts board file (alplabai/alp-zephyr-modules).")
    lines.append(" * The alp,pin-array below is the full 52-entry positional map in")
    lines.append(" * e1m_pinout.h canonical order; fill the <&gpioX Y FLAGS> columns")
    lines.append(" * in place without renumbering (the positional index is the ABI).")
    lines.append(" */")
    lines.append("")

    # v2 short-circuit: a non-Zephyr core has no Zephyr overlay to emit.
    # Customer-passed `--core <id>` may target a yocto / off slice -- the
    # emitter returns a stub so the caller's pipeline doesn't fail.
    if v2_core_os is not None and v2_core_os not in ("zephyr", "baremetal"):
        lines.append(f"// --core {v2_core_id} has os: {v2_core_os}; no Zephyr DTS overlay applies.")
        return "\n".join(lines) + "\n"

    lines.append("#include <zephyr/dt-bindings/gpio/gpio.h>")
    lines.append("")

    sku = project["som"]["sku"]
    # Carrier peripheral DT-wiring catalog for this SoM family.  Computed
    # ONCE here and reused both to skip the generic bus aliases the catalog
    # owns (below) and to emit the catalog fragments at the tail.
    fam = _sku_family(sku)
    wiring = _PERIPH_DT_WIRING.get(fam, {})
    board_name = (board_preset or {}).get("name", "")
    if not board_name:
        lines.append("// No board declared in board.yaml; nothing to emit.")
        return "\n".join(lines) + "\n"

    header_path = _board_header_path(board_name, REPO)
    if not header_path.is_file():
        sys.exit(f"alp_project: no board header at "
                 f"{header_path.relative_to(REPO)} for board '{board_name}' "
                 f"-- DTS overlay emission requires one.")

    macros = _parse_board_macros(header_path)

    lines.append(f"/ {{")
    lines.append(f"    /* Board: {board_name} (SoM SKU {sku}) */")
    lines.append(f"    /* Source: include/alp/boards/{header_path.name} */")
    if v2_peripherals is not None:
        # Surface which Zephyr peripherals the v2 union resolved to so
        # consumers can correlate the alias list back to their cores.
        if v2_core_id is not None:
            lines.append(
                f"    /* v2 scope: --core {v2_core_id} peripherals: "
                f"{', '.join(v2_peripherals) if v2_peripherals else '<none>'} */"
            )
        else:
            lines.append(
                f"    /* v2 scope: union of Zephyr/baremetal cores' peripherals: "
                f"{', '.join(v2_peripherals) if v2_peripherals else '<none>'} */"
            )
    lines.append("")

    # Bus aliases -- one per unique channel the board wires.
    lines.append("    aliases {")
    for class_name, alp_prefix, phandle_prefix in _BUS_BUCKETS:
        # Skip buckets whose peripheral token the catalog owns for this SoM
        # family -- the _PERIPH_DT_WIRING fragment emits the correct alias
        # (e.g. alp-adc0 -> &alp_adc_in0) at the tail, so emitting the generic
        # alp-<x>0 -> &<x>0 here would produce a duplicate / conflicting alias.
        if phandle_prefix in wiring:
            continue
        entries = sorted(set(idx for _macro, idx in macros.get(class_name, [])))
        if not entries:
            continue
        lines.append(f"        /* {class_name} */")
        for idx in entries:
            # Comment lists every board macro that references this channel.
            referencing = [m for (m, i) in macros[class_name] if i == idx]
            comment = ", ".join(referencing)
            lines.append(
                f"        {alp_prefix}{idx} = &{phandle_prefix}{idx};"
                f"  /* {comment} */"
            )
    lines.append("    };")
    lines.append("")

    # alp,pin-array -- the 52-entry positional GPIO map.  Order is fixed
    # by e1m_pinout.h's "Devicetree / overlay invariant" so the GPIO
    # backend's positional resolve (alp_pins[pin_id]) lands on the right
    # pad, including secondary-function pads opened as GPIO via
    # ALP_E1M_GPIO_<class><N>.  Every slot is present even when the board
    # doesn't route it; <&gpioX Y FLAGS> triplets are TBD pending the
    # upstream SoM board file.
    io_by_idx = {idx: m for (m, idx) in macros.get("GPIO_IO", [])}
    pwm_by_idx = {idx: m for (m, idx) in macros.get("PWM", [])}
    canonical = _e1m_gpio_canonical()
    lines.append("    alp_pins: alp-pins {")
    lines.append('        compatible = "alp,pin-array";')
    lines.append("        /* 52 entries in E1M canonical order (e1m_pinout.h).  Indices:")
    lines.append("         *   0..25  IO0..IO25       26..33 PWM0..PWM7")
    lines.append("         *   34..41 ENC0_X..ENC3_Y  42..49 ADC0..ADC7   50..51 DAC0..DAC1")
    lines.append("         * Each <&gpioX Y FLAGS> triplet is TBD pending the upstream SoM")
    lines.append("         * board file; unrouted pads keep their slot so indices stay")
    lines.append("         * stable (alp_gpio_open of an unrouted pad returns NULL).      */")
    lines.append("        gpios =")
    for i, suffix in enumerate(canonical):
        terminator = ";" if i == len(canonical) - 1 else ","
        # Annotate IO / PWM slots with the board macro routed to that pad
        # (parsed from the board header); other classes carry the bare
        # ALP_E1M_GPIO_<suffix> so the customer knows which pad the slot is.
        routed = ""
        if suffix.startswith("IO"):
            n = int(suffix[2:])
            if n in io_by_idx:
                routed = f"  routed: {io_by_idx[n]}"
        elif suffix.startswith("PWM"):
            n = int(suffix[3:])
            if n in pwm_by_idx:
                routed = f"  default fn: {pwm_by_idx[n]}"
        lines.append(
            f"            <&gpio0 0 GPIO_ACTIVE_HIGH>{terminator}"
            f"  /* [{i:2d}] ALP_E1M_GPIO_{suffix}{routed} */"
        )
    lines.append("    };")
    lines.append("")

    lines.append("};")

    # ── carrier peripheral wiring (catalog-driven) ──────────────────────
    # For each declared peripheral carrying a _PERIPH_DT_WIRING entry, append
    # its controller node-enable + the alp-<x>N alias / io-channels consumer
    # the portable backends resolve -- so `peripherals: [adc]` ALONE binds the
    # hardware (no hand-written boards/*.overlay).  The v1 path (v2_peripherals
    # is None) emits nothing here.  A per-example overlay still layers last.
    # `fam`/`wiring` were computed once near the top of the function (and also
    # gate the generic alias skip above), so they are reused here verbatim.
    emitted = [(p, wiring[p]) for p in sorted(set(v2_peripherals or [])) if p in wiring]
    if emitted:
        incs: list[str] = []
        for _p, entry in emitted:
            for inc in entry.get("include", []):
                if inc not in incs:
                    incs.append(inc)
        lines.append("")
        lines.append("/* ---- carrier peripheral wiring "
                     "(auto, from board.yaml `peripherals:`) ---- */")
        for inc in incs:
            lines.append(f"#include <{inc}>")
        if incs:
            lines.append("")
        for p, entry in emitted:
            lines.append(f"/* peripheral: {p} */")
            lines.append(entry["dts"].rstrip("\n"))
            lines.append("")

    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------
# west.yml fragment emission (libraries -> Zephyr-module name-allowlist)
# ---------------------------------------------------------------------
#
# Closes the second v0.4 gap docs/board-config.md flagged: customers
# whose board.yaml declares `libraries: [lvgl, mbedtls]` should not
# also have to hand-add those modules to their app's west.yml
# `name-allowlist:`.  The emitter produces a paste-ready fragment
# they import via a self-referencing `import:` block.


# Library name -> Zephyr module name the workspace's west.yml must
# import.  Mirrors zephyr/modules.git's published modules; LittleFS
# ships as `fs/littlefs` while the rest match their library names 1:1.
_LIBRARY_WEST_MODULES: dict[str, str] = {
    "lvgl":          "lvgl",
    "mbedtls":       "mbedtls",
    "cmsis_dsp":     "cmsis-dsp",
    "littlefs":      "fs/littlefs",
    # The four header-only C++ libraries (etl / fmt / nlohmann_json /
    # doctest) are not Zephyr modules today -- they land in v0.4 via
    # the per-library profile + include-path hook in the loader, not
    # via west.yml.  Listing them here would emit an entry that
    # `west update` rejects.
}


# OTA provider -> Zephyr module name the workspace's west.yml must
# import.  Hawkbit and MCUmgr ship in Zephyr upstream so no entry --
# only out-of-tree clients need a west.yml line.  See ADR 0009.
_OTA_PROVIDER_WEST_MODULES: dict[str, str] = {
    "mender":  "mender-mcu-client",
    # hawkbit -- in Zephyr upstream
    # mcumgr  -- in Zephyr upstream
}


def _load_curated_library_manifest(lib: str) -> dict[str, Any] | None:
    """Load a top-level ADR 0018 library manifest if one exists."""
    path = METADATA_ROOT / "libraries" / f"{lib}.yaml"
    if not path.is_file():
        return None
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    return doc if isinstance(doc, dict) else None


def _emit_west_libraries(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    board_preset: dict[str, Any] | None,
    *,
    v2_libraries: list[str] | None = None,
    v2_project_libraries: list[str] | None = None,
) -> str:
    """Emit a west.yml fragment that the customer's manifest can
    import to pin the Zephyr modules board.yaml's `libraries:` array
    requires.  Idempotent: emitting an empty `libraries:` array gives
    an empty (but well-formed) name-allowlist.

    v1 path (`v2_libraries is None`): reads project-level `libraries:`.
    v2 path: callers compute the union across the Zephyr-runtime cores
    (or pick one when `--core <id>` is supplied) and pass it in via
    `v2_libraries`.  `v2_project_libraries` carries the top-level ADR 0018
    curated library manifests; these may either import a Zephyr-owned module
    by name or emit a standalone west project pin from the manifest's
    `integration.zephyr.west` block.
    """
    del sku_preset, board_preset  # unused -- libraries are SoM-agnostic
    if v2_libraries is not None:
        libs = list(v2_libraries)
    else:
        libs = project.get("libraries") or []
    project_libs = list(v2_project_libraries
                        if v2_project_libraries is not None
                        else [])
    modules: list[tuple[str, str]] = []   # (library, Zephyr-owned west module)
    west_projects: list[tuple[str, dict[str, Any]]] = []
    unsupported: list[str] = []
    seen_modules: set[str] = set()
    seen_projects: set[str] = set()

    def add_module(lib: str, mod: str) -> None:
        if mod not in seen_modules:
            modules.append((lib, mod))
            seen_modules.add(mod)

    def add_west_project(lib: str, west: dict[str, Any]) -> None:
        name = str(west.get("name") or "")
        if name and name not in seen_projects:
            west_projects.append((lib, west))
            seen_projects.add(name)

    for lib in libs:
        mod = _LIBRARY_WEST_MODULES.get(lib)
        if mod is None:
            unsupported.append(lib)
        else:
            add_module(lib, mod)

    for lib in project_libs:
        manifest = _load_curated_library_manifest(lib)
        zephyr = ((manifest or {}).get("integration") or {}).get("zephyr") or {}
        if not zephyr:
            continue
        west = zephyr.get("west")
        if isinstance(west, dict):
            add_west_project(lib, west)
            continue
        mod = zephyr.get("module")
        if isinstance(mod, str) and mod:
            add_module(lib, mod)
        else:
            unsupported.append(lib)

    # OTA provider-driven dispatch (ADR 0009 follow-up): out-of-tree
    # Zephyr OTA clients need their own west.yml entry.  Mender-MCU-client
    # is the only one today; hawkbit and mcumgr ship in Zephyr upstream.
    ota = project.get("ota") or {}
    if isinstance(ota, dict):
        ota_provider = (ota.get("provider") or "").lower()
        ota_mod = _OTA_PROVIDER_WEST_MODULES.get(ota_provider)
        if ota_mod is not None:
            modules.append((f"ota:{ota_provider}", ota_mod))

    lines: list[str] = []
    lines.append("# SPDX-License-Identifier: Apache-2.0")
    lines.append("#")
    lines.append("# Auto-generated by scripts/alp_project.py -- "
                 "do not edit by hand.")
    lines.append("# Regenerate after changes to board.yaml's `libraries:` array.")
    lines.append("#")
    lines.append("# Import into your application's west.yml so `west update`")
    lines.append("# pulls only the Zephyr modules the libraries you enabled")
    lines.append("# actually need.  Drop alongside your west.yml and reference")
    lines.append("# from the `import:` field of the alp-sdk project entry.")
    lines.append("")
    lines.append("manifest:")
    lines.append("  projects:")
    lines.append("    - name: zephyr")
    lines.append("      import:")
    lines.append("        name-allowlist:")
    if modules:
        for lib, mod in modules:
            lines.append(f"          - {mod}        # board.yaml libraries: '{lib}'")
    else:
        lines.append("          # no selected Zephyr-owned modules -- nothing to allowlist.")
        lines.append("          []")

    if west_projects:
        lines.append("")
        lines.append("    # ADR 0018 libraries not imported by Zephyr's own west.yml.")
        for lib, west in west_projects:
            lines.append(f"    - name: {west['name']}")
            lines.append(f"      url: {west['url']}")
            lines.append(f"      revision: {west['revision']}")
            lines.append(f"      path: {west['path']}        # board.yaml libraries: '{lib}'")

    if unsupported:
        lines.append("")
        lines.append("# The following libraries have no Zephyr west project entry today")
        lines.append("# (header-only/profile libraries ride the loader's include path;")
        lines.append("# Yocto-only or in-tree Zephyr subsystems do not need a project pin):")
        for lib in unsupported:
            lines.append(f"#   - {lib}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------
# C header emission (build-time identifiers for <alp/hw_info.h>)
# ---------------------------------------------------------------------
#
# Produces the auto-generated `<alp_hw_info_build.h>` companion to
# `<alp/hw_info.h>` -- a small header that bakes the customer's
# board.yaml identifiers in as `ALP_HW_BUILD_*` string macros so the
# runtime check has something to compare the EEPROM read against:
#
#     #include "alp/hw_info.h"
#     #include "alp_hw_info_build.h"   // generated
#
#     alp_hw_info_t info;
#     alp_hw_info_read(&info);
#     alp_hw_info_assert_matches_build(&info,
#                                      ALP_HW_BUILD_SOM_SKU,
#                                      ALP_HW_BUILD_SOM_HW_REV);
#
# The CMakeLists.txt example pattern (mirroring the zephyr-conf
# emission) writes the header to `${CMAKE_BINARY_DIR}/generated/`
# and adds that path to the include search.


def _pick_primary_core_os(cores: dict[str, str]) -> tuple[str, str]:
    """Pick the "primary" core for the `ALP_HW_BUILD_OS` macro.

    `cores` maps core id -> os string ("zephyr" / "yocto" / "baremetal" /
    "off").  The selection rule:

      1. First M-class core (alphabetical by id), with os != off, if any.
      2. Else first A-class core (alphabetical by id), with os != off, if any.
      3. Else first non-off core (alphabetical by id), if any.
      4. Else returns ("", "off").

    Returns (core_id, os).
    """
    active = {cid: os_ for cid, os_ in cores.items() if os_ != "off"}
    if not active:
        return ("", "off")
    m_class = sorted(cid for cid in active if cid.startswith("m"))
    if m_class:
        cid = m_class[0]
        return (cid, active[cid])
    a_class = sorted(cid for cid in active if cid.startswith("a"))
    if a_class:
        cid = a_class[0]
        return (cid, active[cid])
    cid = sorted(active.keys())[0]
    return (cid, active[cid])


def _emit_hw_info_h(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    board_preset: dict[str, Any] | None,
    *,
    v2_cores: dict[str, str] | None = None,
    v2_selected_core: str | None = None,
) -> str:
    """Emit <alp_hw_info_build.h> -- build-time identifier companion to
    <alp/hw_info.h>.

    v1 path (`v2_cores is None`): the `ALP_HW_BUILD_OS` macro comes from
    `project.os` (the v1 schema's single top-level OS).

    v2 path: derive `ALP_HW_BUILD_OS` from the cores: block.  If
    `v2_selected_core` is set (i.e. the caller passed `--core <id>`),
    use that core's OS.  Else pick a "primary" core via
    `_pick_primary_core_os`: first M-class core alphabetically, falling
    back to first A-class core, falling back to any non-off core.

    The v2 path also emits `ALP_HW_BUILD_CORES` (comma-separated list of
    every non-off core id) and one `ALP_HW_BUILD_HAS_<id>` macro per
    non-off core so consumers can `#ifdef` on the topology.
    """
    sku = project["som"]["sku"]
    som_hw_rev = (project["som"].get("hw_rev")
                  or sku_preset.get("default_hw_rev")
                  or "unknown")
    family = _sku_family(sku)

    board_block = project.get("board") or {}
    board_name = board_block.get("name") or ""
    board_hw_rev = ""
    if board_name and board_preset is not None:
        board_hw_rev = (board_block.get("hw_rev")
                          or board_preset.get("default_hw_rev")
                          or "")

    # Resolve the OS string.
    primary_core_id = ""
    primary_core_os = ""
    if v2_cores is not None:
        if v2_selected_core is not None and v2_selected_core in v2_cores:
            primary_core_id = v2_selected_core
            primary_core_os = v2_cores[v2_selected_core]
        else:
            primary_core_id, primary_core_os = _pick_primary_core_os(v2_cores)
        os_choice = primary_core_os
    else:
        os_choice = project.get("os") or ""

    lines: list[str] = [
        "/*",
        " * Auto-generated by scripts/alp_project.py -- do not edit by hand.",
        " * Regenerate after changes to board.yaml.",
        " *",
        " * Build-time identifier companion to <alp/hw_info.h>.  Apps include",
        " * this header alongside <alp/hw_info.h> and pass the ALP_HW_BUILD_*",
        " * string macros to alp_hw_info_assert_matches_build() so the runtime",
        " * EEPROM read can be checked against what the firmware was built for.",
        " */",
        "",
        "#ifndef ALP_HW_INFO_BUILD_H",
        "#define ALP_HW_INFO_BUILD_H",
        "",
        f'#define ALP_HW_BUILD_SOM_SKU         "{sku}"',
        f'#define ALP_HW_BUILD_SOM_FAMILY      "{family}"',
        f'#define ALP_HW_BUILD_SOM_HW_REV      "{som_hw_rev}"',
    ]
    if board_name:
        lines.append(f'#define ALP_HW_BUILD_BOARD_NAME      "{board_name}"')
        if board_hw_rev:
            lines.append(f'#define ALP_HW_BUILD_BOARD_HW_REV    "{board_hw_rev}"')
    if os_choice:
        lines.append(f'#define ALP_HW_BUILD_OS              "{os_choice}"')
    if v2_cores is not None:
        # Per-core topology surface for `#ifdef ALP_HW_BUILD_HAS_<id>`
        # conditional compilation.  Primary-core selection rule:
        #   1. First M-class core (alphabetical by id), if any non-off.
        #   2. Else first A-class core (alphabetical by id), if any non-off.
        #   3. Else first non-off core (alphabetical by id).
        active = sorted(cid for cid, os_ in v2_cores.items() if os_ != "off")
        if active:
            lines.append("")
            lines.append(
                f'#define ALP_HW_BUILD_CORES           "{",".join(active)}"'
            )
            if primary_core_id:
                lines.append(
                    f'#define ALP_HW_BUILD_PRIMARY_CORE    "{primary_core_id}"'
                )
            lines.append("")
            lines.append("/* Per-core presence flags -- `#ifdef "
                         "ALP_HW_BUILD_HAS_<id>` to compile slice-")
            lines.append(" * specific code.  Each macro's value is the "
                         "slice's OS string, useful for")
            lines.append(" * `#if defined(...)`-style runtime selection. */")
            for cid in active:
                lines.append(
                    f'#define ALP_HW_BUILD_HAS_{cid.upper():<12} "{v2_cores[cid]}"'
                )
    lines += [
        "",
        "#endif /* ALP_HW_INFO_BUILD_H */",
        "",
    ]
    return "\n".join(lines)


# ---------------------------------------------------------------------
# Carrier route / netlist emitters
# ---------------------------------------------------------------------
#
# `composed-route-table` stays as the original debug surface.  The
# route-row helper below is shared by the production `carrier-netlist`
# contract so the two views cannot drift on hw_rev pad-route overrides.


def _composed_route_rows(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    board_preset: dict[str, Any] | None,
    metadata_root: Path,
) -> tuple[list[dict[str, Any]], str | None, str | None]:
    """Return composed route rows plus the selected hw_rev / variant.

    Rows cover every E1M pad named by the board and every SoM-only pad
    that has a dispatch route.  Board-defined rows preserve YAML order;
    SoM-only rows are sorted by E1M ID for deterministic output.
    """
    pad_routes = _resolve_pad_routes(sku_preset)

    # Apply the selected board revision's pad-route overrides on top of the
    # base (production-rev) pad_routes, so the composed table -- and thus
    # `--emit composed-route-table` -- differs by hw_rev.  The rev comes from
    # the board's `som.hw_rev`, falling back to the SoM's `default_hw_rev`.
    hw_rev = ((project.get("som") or {}).get("hw_rev")
              or sku_preset.get("default_hw_rev"))
    for ov in _hwrev_pad_route_overrides(project["som"]["sku"], hw_rev,
                                         metadata_root):
        pad_routes[ov["e1m"]] = ov

    # Resolve silicon variant order_code for the top-level summary field.
    variant = _resolve_silicon_variant(sku_preset, metadata_root)
    silicon_variant_str = variant["order_code"] if variant else None

    # Collect board-side entries, preserving the sub-category name.
    # Build a mapping: e1m_id -> (category, entry_dict).
    # When the same E1M pad appears multiple times (e.g. E1M_PWM1 maps to
    # both EVK_PWM_LED_BLUE and EVK_ARD_PWM1 in the EVK YAML) we emit one
    # row per board entry so no information is lost.
    board_entries: list[tuple[str, dict[str, Any]]] = []
    seen_from_board: set[str] = set()
    if board_preset is not None:
        e1m_routes = board_preset.get("e1m_routes") or {}
        for category, entries in e1m_routes.items():
            if not isinstance(entries, list):
                continue
            for entry in entries:
                if not isinstance(entry, dict):
                    continue
                e1m = entry.get("e1m")
                if not isinstance(e1m, str):
                    continue
                board_entries.append((category, entry))
                seen_from_board.add(e1m)

    # Also include SoM-only pads (in pad_routes but not in board).
    som_only_pads = sorted(set(pad_routes.keys()) - seen_from_board)

    routes: list[dict[str, Any]] = []

    # Board-defined entries first (preserves YAML order).
    for category, c_entry in board_entries:
        e1m = c_entry["e1m"]
        composed = _compose_route(e1m, c_entry, pad_routes)
        row: dict[str, Any] = {"e1m": e1m, "board_category": category}
        row["board_macro"] = composed.get("board_macro")
        row["board_role"] = composed.get("board_role")
        if "board_doc" in composed:
            row["board_doc"] = composed["board_doc"]
        # active_low is a board-side flag, not surfaced by _compose_route;
        # read it directly from the board entry.
        active_low = c_entry.get("active_low")
        if active_low is not None:
            row["active_low"] = bool(active_low)
        row["dispatch"] = composed.get("dispatch", "direct")
        if "dispatch_pin" in composed:
            row["dispatch_pin"] = composed["dispatch_pin"]
        if "som_doc" in composed:
            row["som_doc"] = composed["som_doc"]
        routes.append(row)

    # SoM-only pads (not assigned a board role in this board YAML).
    for e1m in som_only_pads:
        composed = _compose_route(e1m, None, pad_routes)
        row = {
            "e1m": e1m,
            "board_category": None,
            "board_macro": None,
            "board_role": None,
            "dispatch": composed.get("dispatch", "direct"),
        }
        if "dispatch_pin" in composed:
            row["dispatch_pin"] = composed["dispatch_pin"]
        if "som_doc" in composed:
            row["som_doc"] = composed["som_doc"]
        routes.append(row)

    return routes, hw_rev, silicon_variant_str


def _emit_composed_route_table(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    board_preset: dict[str, Any] | None,
    metadata_root: Path,
) -> str:
    """Emit a JSON summary of the fully-composed pad route table for
    the current (board x SoM) pair.

    The table is derived by calling _resolve_pad_routes() (SoM side) and
    _compose_route() (join with board side) for every E1M pad that
    appears in either the board's e1m_routes: block or the SoM's
    pad_routes: block.

    Pads that only appear in the SoM's pad_routes: block (i.e. no
    board-side role assigned) are included with null board_* fields
    so the table is complete for the SoM-standalone scenario.
    """
    routes, hw_rev, silicon_variant_str = _composed_route_rows(
        project, sku_preset, board_preset, metadata_root)
    board_name = (board_preset or {}).get("name") or project.get("name")
    result: dict[str, Any] = {
        "board": board_name,
        "som": project["som"]["sku"],
        "hw_rev": hw_rev,
        "silicon_variant": silicon_variant_str,
        "routes": routes,
    }
    return json.dumps(result, indent=2) + "\n"


def _manifest_path(kind: str, item_id: str, metadata_root: Path) -> Path:
    return metadata_root / kind / f"{item_id}.yaml"


def _load_optional_manifest(kind: str, item_id: str,
                            metadata_root: Path) -> dict[str, Any] | None:
    path = _manifest_path(kind, item_id, metadata_root)
    if not path.is_file():
        return None
    return _load_yaml(path)


def _passive_rows(passives: list[dict[str, Any]] | None) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for passive in passives or []:
        if not isinstance(passive, dict):
            continue
        row: dict[str, Any] = {
            "role": passive.get("role"),
            "value": passive.get("value"),
            "net": passive.get("net"),
            "refdes_prefix": passive.get("refdes_prefix"),
        }
        rows.append({k: v for k, v in row.items() if v is not None})
    return rows


def _chip_bom_row(item_id: str, manifest: dict[str, Any],
                  manifest_relpath: str) -> dict[str, Any]:
    physical = manifest.get("physical") or {}
    caveats: list[str] = []
    if not physical:
        caveats.append("missing_physical")
    elif physical.get("visibility") == "internal":
        caveats.append("physical_detail_internal")
    if len(manifest.get("mpn_population") or []) > 1:
        caveats.append("mpn_population_candidates")

    row: dict[str, Any] = {
        "item_id": item_id,
        "kind": "chip",
        "scope": "carrier",
        "source": manifest_relpath,
        "display_name": manifest.get("display_name"),
        "vendor": manifest.get("vendor"),
        "mpn_population": manifest.get("mpn_population") or [],
        "bus": manifest.get("bus"),
        "quantity": None,
        "physical": {
            "refdes_prefix": physical.get("refdes_prefix"),
            "package": physical.get("package"),
            "footprint": physical.get("footprint"),
            "visibility": physical.get("visibility"),
            "provenance": physical.get("provenance"),
        },
        "passives": _passive_rows(physical.get("passives")),
    }
    row["physical"] = {k: v for k, v in row["physical"].items()
                       if v is not None}
    if caveats:
        row["caveats"] = caveats
    return row


def _block_bom_row(item_id: str, manifest: dict[str, Any],
                   manifest_relpath: str) -> dict[str, Any]:
    realizations = [
        r for r in manifest.get("realizations") or []
        if isinstance(r, dict)
    ]
    realization = realizations[0] if realizations else {}
    caveats: list[str] = []
    if not realizations:
        caveats.append("missing_realization")
    elif len(realizations) > 1:
        caveats.append("multiple_realizations")
    if realization.get("visibility") == "internal":
        caveats.append("physical_detail_internal")
    if not realization.get("parts"):
        caveats.append("no_concrete_parts")

    row: dict[str, Any] = {
        "item_id": item_id,
        "kind": "block",
        "scope": "carrier",
        "source": manifest_relpath,
        "display_name": manifest.get("display_name"),
        "quantity": None,
        "realization": {
            "id": realization.get("id"),
            "physical_form": realization.get("physical_form"),
            "visibility": realization.get("visibility"),
        },
        "parts": realization.get("parts") or [],
        "passives": _passive_rows(realization.get("passives")),
    }
    row["realization"] = {k: v for k, v in row["realization"].items()
                          if v is not None}
    if caveats:
        row["caveats"] = caveats
    return row


def _carrier_bom_rows(
    board_preset: dict[str, Any] | None,
    metadata_root: Path,
) -> list[dict[str, Any]]:
    """Build carrier BOM rows from board `populated: true`.

    `populated:` is a logical population map, not a line-item BOM with
    refdes or count, so rows deliberately leave `quantity` null unless a
    future metadata field makes it authoritative.
    """
    rows: list[dict[str, Any]] = []
    if board_preset is None:
        return rows

    populated = board_preset.get("populated") or {}
    for item_id in sorted(k for k, v in populated.items() if v is True):
        chip = _load_optional_manifest("chips", item_id, metadata_root)
        if chip is not None:
            rows.append(_chip_bom_row(
                item_id, chip, f"metadata/chips/{item_id}.yaml"))
            continue

        block = _load_optional_manifest("blocks", item_id, metadata_root)
        if block is not None:
            rows.append(_block_bom_row(
                item_id, block, f"metadata/blocks/{item_id}.yaml"))
            continue

        rows.append({
            "item_id": item_id,
            "kind": "unknown",
            "scope": "carrier",
            "source": None,
            "quantity": None,
            "caveats": ["missing_manifest"],
        })
    return rows


def _route_to_net(row: dict[str, Any]) -> dict[str, Any]:
    net_id = row.get("board_macro") or row["e1m"]
    endpoints: list[dict[str, Any]] = [
        {"kind": "e1m", "ref": row["e1m"]},
    ]
    if row.get("board_macro"):
        endpoints.append({"kind": "board-macro", "ref": row["board_macro"]})
    if row.get("dispatch") and row["dispatch"] != "direct":
        endpoint: dict[str, Any] = {
            "kind": "som-dispatch",
            "ref": row["dispatch"],
        }
        if "dispatch_pin" in row:
            endpoint["pin"] = row["dispatch_pin"]
        endpoints.append(endpoint)

    net: dict[str, Any] = {
        "net_id": net_id,
        "e1m": row["e1m"],
        "board_category": row.get("board_category"),
        "board_macro": row.get("board_macro"),
        "board_role": row.get("board_role"),
        "dispatch": row.get("dispatch", "direct"),
        "endpoints": endpoints,
    }
    for key in ("board_doc", "active_low", "dispatch_pin", "som_doc"):
        if key in row:
            net[key] = row[key]
    caveats = []
    if row.get("board_macro") is None:
        caveats.append("som_only_no_carrier_role")
    if caveats:
        net["caveats"] = caveats
    return net


def _emit_carrier_netlist(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    board_preset: dict[str, Any] | None,
    metadata_root: Path,
) -> str:
    """Emit the Studio-facing carrier netlist + BOM handoff contract.

    This is intentionally not a KiCad, Gerber, or layout artifact.  It
    exposes only public carrier-facing facts derivable from board.yaml,
    board presets, chip/block manifests, and SoM pad dispatch metadata.
    """
    routes, hw_rev, silicon_variant_str = _composed_route_rows(
        project, sku_preset, board_preset, metadata_root)
    board_name = (board_preset or {}).get("name") or project.get("name")
    result: dict[str, Any] = {
        "schema_version": 1,
        "kind": "alp.carrier_netlist",
        "generated_by": "scripts/alp_project.py --emit carrier-netlist",
        "board": board_name,
        "som": project["som"]["sku"],
        "hw_rev": hw_rev,
        "silicon_variant": silicon_variant_str,
        "nets": [_route_to_net(row) for row in routes],
        "bom": {
            "carrier": _carrier_bom_rows(board_preset, metadata_root),
        },
        "caveats": [
            "carrier_handoff_not_pcb_layout",
            "no_kicad_or_gerber_output",
            "som_internals_excluded",
            "quantity_null_when_board_populated_has_no_count",
        ],
    }
    return json.dumps(result, indent=2) + "\n"


# ---------------------------------------------------------------------
# v2 emit shims
# ---------------------------------------------------------------------
#
# The orchestrator (scripts/alp_orchestrate/) owns the v2 board.yaml
# loader + carve-out resolver + system-manifest emitter.  These shims
# route the v2-only `--emit` modes (and the per-core
# `--emit zephyr-conf --core <id>`) through the orchestrator.


def _write_or_print(out: str, target: Path | None) -> int:
    if target is not None:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(out, encoding="utf-8")
        try:
            rel = target.relative_to(Path.cwd())
        except ValueError:
            rel = target
        print(f"alp_project: wrote {rel} ({len(out)} bytes)",
              file=sys.stderr)
    else:
        sys.stdout.write(out)
    return 0


def _run_v2_emit(args: argparse.Namespace) -> int:
    """Handle the three project-level v2 emit modes."""
    if args.core is not None:
        print(f"alp_project: --core is ignored for --emit {args.emit} "
              f"(project-level emit)", file=sys.stderr)
    try:
        # Imported here so the v1 path doesn't pay the import cost when
        # the orchestrator module is being modified in-tree.
        from alp_orchestrate import (
            OrchestratorError,
            emit_dts_reservations,
            emit_ipc_contract_h,
            emit_os_topology,
            emit_system_manifest,
            load_board_yaml,
        )
    except ImportError as e:
        print(f"alp_project: failed to import alp_orchestrate: {e}",
              file=sys.stderr)
        return 1

    try:
        project = load_board_yaml(args.input,
                                  metadata_root=args.metadata_root)
        if args.emit == "system-manifest":
            out = emit_system_manifest(project)
        elif args.emit == "ipc-contract-h":
            out = emit_ipc_contract_h(project)
        elif args.emit == "dts-reservations":
            out = emit_dts_reservations(project)
        elif args.emit == "os-topology":
            out = emit_os_topology(project)
        else:
            print(f"alp_project: unknown v2 emit '{args.emit}'",
                  file=sys.stderr)
            return 1
    except OrchestratorError as e:
        print(f"alp_project: {e}", file=sys.stderr)
        return 1

    return _write_or_print(out, args.output)


def _run_v2_per_core_emit(args: argparse.Namespace) -> int:
    """v2 board.yaml + per-core --emit zephyr-conf / yocto-conf, plus the
    project-wide legacy emit modes (`dts-overlay`, `hw-info-h`,
    `west-libraries`) re-fitted for the v2 schema.

    The orchestrator owns the per-slice config emitters; this shim
    delegates after resolving the requested core (or summing across
    cores when `--core` is unset).
    """
    try:
        from alp_orchestrate import (
            OrchestratorError,
            _slice_alp_conf,
            _slice_cmake_args,
            _slice_local_conf,
            load_board_yaml,
        )
    except ImportError as e:
        print(f"alp_project: failed to import alp_orchestrate: {e}",
              file=sys.stderr)
        return 1

    try:
        project = load_board_yaml(args.input,
                                  metadata_root=args.metadata_root)
    except OrchestratorError as e:
        print(f"alp_project: {e}", file=sys.stderr)
        return 1

    # Validate --core if supplied (used by every emit path).
    if args.core is not None and args.core not in project.cores:
        print(f"alp_project: --core {args.core} not present in "
              f"board.yaml (known: {sorted(project.cores.keys())})",
              file=sys.stderr)
        return 1

    # Build a dict in the legacy "board:"-wrapper shape that the
    # in-file emitters still consume internally (dts-overlay,
    # hw-info-h, west-libraries).  The public board.yaml schema no
    # longer uses this wrapper, but it's a convenient internal
    # representation for the emitters' read paths.
    project_v1_shaped: dict[str, Any] = {
        "som": {
            "sku":    project.sku,
            "hw_rev": project.hw_rev,
        },
        "board": ({
            "name":   project.board_name,
            "hw_rev": project.board_hw_rev,
        } if project.board_name else None),
    }

    # --- legacy project-wide emits, v2-flavoured -------------------------
    if args.emit == "dts-overlay":
        # The DTS overlay is shaped by the board header (bus aliases +
        # alp,pin-array) which is a SoM-mounting fact, not a per-core
        # fact.  v2 contributes only the peripherals list: union across
        # Zephyr/baremetal cores (or one core when --core is set).
        if args.core is not None:
            slice_ = project.cores[args.core]
            v2_peripherals = sorted(set(slice_.peripherals))
            out = _emit_dts_overlay(
                project_v1_shaped, project.som_preset,
                project.board_preset,
                v2_peripherals=v2_peripherals,
                v2_core_id=args.core,
                v2_core_os=slice_.os,
            )
        else:
            union: set[str] = set()
            for slice_ in project.cores.values():
                if slice_.os in ("zephyr", "baremetal"):
                    union.update(slice_.peripherals)
            out = _emit_dts_overlay(
                project_v1_shaped, project.som_preset,
                project.board_preset,
                v2_peripherals=sorted(union),
            )
        return _write_or_print(out, args.output)

    if args.emit == "hw-info-h":
        # hw-info-h is a project-level emit even under v2 -- consumers
        # `#include` it from any slice.  --core picks which slice's OS
        # lands in ALP_HW_BUILD_OS; absent --core, primary-core rules apply.
        v2_cores = {cid: s.os for cid, s in project.cores.items()}
        out = _emit_hw_info_h(
            project_v1_shaped, project.som_preset,
            project.board_preset,
            v2_cores=v2_cores,
            v2_selected_core=args.core,
        )
        return _write_or_print(out, args.output)

    if args.emit == "west-libraries":
        if args.core is not None:
            slice_ = project.cores[args.core]
            v2_libraries = sorted(set(slice_.libraries))
        else:
            union_l: set[str] = set()
            for slice_ in project.cores.values():
                if slice_.os in ("zephyr", "baremetal"):
                    union_l.update(slice_.libraries)
            v2_libraries = sorted(union_l)
        out = _emit_west_libraries(
            project_v1_shaped, project.som_preset,
            project.board_preset,
            v2_libraries=v2_libraries,
            v2_project_libraries=sorted(project.libraries),
        )
        return _write_or_print(out, args.output)

    # --- per-core emits (zephyr-conf / yocto-conf / cmake-args) ----------
    #
    # If --core is unset, sum across cores.  Per spec §4.6, the new
    # per-core invocation is the canonical entry point; the unscoped
    # invocation is a sum-across-cores convenience for tools that
    # haven't moved off the v1 single-OS world yet.
    if args.core is not None:
        core_ids = [args.core]
    else:
        core_ids = sorted(project.cores.keys())

    # Resolve + compatibility-validate any top-level `libraries:` once, up
    # front, so an unknown name or a failed `requires:` constraint surfaces
    # as a clean one-line error (ADR 0018) rather than a traceback mid-emit.
    if project.libraries:
        try:
            from alp_orchestrate.libraries import resolve_selection
            resolve_selection(project, args.metadata_root)
        except OrchestratorError as e:
            print(f"alp_project: {e}", file=sys.stderr)
            return 1

    parts: list[str] = []
    for cid in core_ids:
        slice_ = project.cores[cid]
        if slice_.os == "off":
            continue
        if args.emit == "zephyr-conf":
            if slice_.os != "zephyr":
                # When --core is unset, filter by os: zephyr; with --core,
                # honour the explicit selection but warn.
                if args.core is None:
                    continue
                print(f"alp_project: --core {cid} has os: {slice_.os}; "
                      f"emitting Kconfig fragment anyway", file=sys.stderr)
            parts.append(f"# --- core: {cid} ({slice_.os}) ---")
            parts.append(_slice_alp_conf(project, slice_))
            hw_lines = _emit_library_hw_backends(slice_.libraries, project.sku)
            if hw_lines:
                parts.append("# §D.lib.loader -- per-library HW-accelerator wiring (auto-emitted).")
                parts.extend(hw_lines)
                parts.append("")
        elif args.emit == "yocto-conf":
            if slice_.os != "yocto":
                if args.core is None:
                    continue
                print(f"alp_project: --core {cid} has os: {slice_.os}; "
                      f"emitting local.conf snippet anyway", file=sys.stderr)
            parts.append(f"# --- core: {cid} ({slice_.os}) ---")
            parts.append(_slice_local_conf(project, slice_))
        elif args.emit == "cmake-args":
            if slice_.os not in ("baremetal", "zephyr"):
                if args.core is None:
                    continue
            parts.append(f"# --- core: {cid} ({slice_.os}) ---")
            parts.append(_slice_cmake_args(project, slice_))
        else:
            print(f"alp_project: unknown --emit {args.emit} for v2 board.yaml",
                  file=sys.stderr)
            return 1

    out = "\n".join(parts) + ("\n" if parts and not parts[-1].endswith("\n") else "")
    return _write_or_print(out, args.output)


# ---------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description="Compile board.yaml -> per-backend native config.")
    parser.add_argument("--input", type=Path, default=Path("board.yaml"),
                        help="Path to the project's board.yaml (default: ./board.yaml).")
    parser.add_argument("--emit",
                        choices=["zephyr-conf", "cmake-args", "yocto-conf",
                                 "dts-overlay", "hw-info-h", "west-libraries",
                                 # v2 orchestration emits (Phase 2):
                                 "system-manifest", "dts-reservations",
                                 "ipc-contract-h",
                                 # Per-core natural-vs-effective OS facts (issue #95).
                                 "os-topology",
                                 # Carrier routing / Studio handoff JSON.
                                 "composed-route-table",
                                 "carrier-netlist"],
                        default="zephyr-conf",
                        help="Output format (default: zephyr-conf).")
    parser.add_argument("--output", type=Path, default=None,
                        help="Write to this path; default: stdout.")
    parser.add_argument("--metadata-root", type=Path, default=METADATA_ROOT,
                        help="Override the metadata search root.")
    parser.add_argument("--core", default=None,
                        help="When the project is v2, limit emits to this "
                             "core ID.  For per-core emit modes "
                             "(zephyr-conf, yocto-conf, cmake-args) this "
                             "picks the single slice to emit.  For "
                             "project-wide emit modes (dts-overlay, "
                             "hw-info-h, west-libraries) this scopes the "
                             "union calculation to a single slice (e.g. "
                             "ALP_HW_BUILD_OS reflects the selected core's "
                             "runtime).  Ignored for system-manifest, "
                             "ipc-contract-h, dts-reservations.")
    args = parser.parse_args()

    # Project-wide v2 emit modes (system-manifest, dts-reservations,
    # ipc-contract-h) route through alp_orchestrate/ directly.
    if args.emit in ("system-manifest", "dts-reservations",
                     "ipc-contract-h", "os-topology"):
        return _run_v2_emit(args)

    project = _validate_and_load(args.input)

    # composed-route-table / carrier-netlist only need the SoM + board
    # definitions; they do not require the per-core slice machinery.
    if args.emit in ("composed-route-table", "carrier-netlist"):
        sku_preset_rt = _resolve_sku(project["som"]["sku"], args.metadata_root)
        board_preset_rt = _resolve_inline_or_preset_board(
            project, args.metadata_root)
        if args.emit == "composed-route-table":
            out = _emit_composed_route_table(
                project, sku_preset_rt, board_preset_rt, args.metadata_root
            )
        else:
            out = _emit_carrier_netlist(
                project, sku_preset_rt, board_preset_rt, args.metadata_root
            )
        return _write_or_print(out, args.output)

    # board.yamls flow through the per-core / project-wide emit path
    # in _run_v2_per_core_emit.  Project-wide emits (system-manifest,
    # dts-reservations, ipc-contract-h) were already dispatched above.
    return _run_v2_per_core_emit(args)


if __name__ == "__main__":
    sys.exit(main())
