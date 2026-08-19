#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
board.yaml loader -- SKU/board/pad-route resolution, memory-map and
capability derivation for `scripts/alp_project.py`.

Split out of the former ~2500-line `alp_project.py` monolith (issue #459):
this module owns everything that turns on-disk metadata (SoM presets,
board presets, SoC JSON, hw-revisions) into the plain dicts the emitters
in `alp_project_emit.py` render.  `alp_project.py` stays the CLI entry
point and re-exports the public/compat names from here so existing
`import alp_project` / `from alp_project import <name>` call sites (the
west commands, `alp_orchestrate/`, and the test suite) keep working
unchanged -- this is a structural split only, no behaviour change.
"""

from __future__ import annotations

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

from sentinels import is_tbd


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


def _sku_form_factor(sku: str) -> str:
    """Return the E1M connector family for a SKU string."""
    return "e1m-x" if _sku_family(sku) in {"v2n", "v2n-m1"} else "e1m"


# Silicon ref -> Zephyr SoC-select Kconfig symbol.
#
# This is NOT a hand-maintained table: the symbol is computed from the
# ref, and the single source of the allowlist is the versioned registry
# at metadata/registries/silicon-kconfig.json.  Both this emitter and
# scripts/alp_orchestrate/ consume `silicon_to_kconfig()` so the
# mapping has exactly one definition (the prior _SILICON_TO_KCONFIG dict
# was duplicated across both files -- "duplicated truth is a bug").


@functools.lru_cache(maxsize=None)
def _load_silicon_kconfig(metadata_root: Path) -> tuple[str, frozenset[str]]:
    """Load (socSymbolPrefix, knownSilicon) from *metadata_root*'s versioned
    registry. Cache is keyed on *metadata_root* so a second root in the
    same process doesn't silently reuse the first root's table (#1485)."""
    registry = Path(metadata_root) / "registries" / "silicon-kconfig.json"
    data = json.loads(registry.read_text(encoding="utf-8"))
    return data["socSymbolPrefix"], frozenset(data["knownSilicon"])


def silicon_to_kconfig(silicon: str | None, metadata_root: Path) -> str | None:
    """Return the Zephyr Kconfig symbol that selects *silicon*, or ``None``.

    The symbol is computed as ``socSymbolPrefix + ref.upper().replace(':','_')``;
    e.g. ``alif:ensemble:e7`` -> ``ALP_SOC_ALIF_ENSEMBLE_E7``.  ``None`` is
    returned for any ref not in the registry allowlist (so an accelerator
    such as ``deepx:dx:m1`` -- or an unknown ref -- emits no CONFIG line,
    matching the prior dict's ``.get()`` behaviour).

    *metadata_root* is REQUIRED -- callers must pass the project's own
    ``project.effective_metadata_root()``, never a module default: a
    silent fall-through to the SDK's own in-tree registry regardless of a
    project's `--metadata-root` override was #1485's exact defect.
    """
    if silicon is None:
        return None
    prefix, known = _load_silicon_kconfig(metadata_root)
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


def _validate_and_load(
    path: Path, metadata_root: Path = METADATA_ROOT
) -> dict[str, Any]:
    """Validate *path* with the rich diagnostic validator, then return a
    plain ``dict`` for the downstream emitters.

    *metadata_root* threads the CLI's `--metadata-root` override into the
    rich validator's SoM/preset/SoC lookups (#604) -- without it a
    customer's out-of-tree metadata copy validated fine via
    `load_board_yaml(metadata_root=...)` downstream but failed here first
    against the repo's own metadata.

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

    collector = validate_board_yaml(path, metadata_root=metadata_root)
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


def split_silicon_ref(silicon: str | None) -> tuple[str, str, str] | None:
    """Split a `vendor:family:part` `silicon:` key into its three slugs.

    Returns None when `silicon` is falsy or not exactly 3 colon-separated
    parts -- the same guard `resolve_soc_path()` applies, because that
    function is now a thin rooting convenience over this one.

    This exists because the SPLIT, not the rooting, is what kept getting
    hand-rolled (issues #997, #1004, #1096). Every site that re-derived it
    rooted the result somewhere different -- at a caller-injected `soc_dir`,
    at `metadata_root`, at an `output_root`, or at no filesystem path at all
    (a repo-relative `str` for a generated comment) -- so a single
    path-returning helper could never absorb them all, and four survived
    two rounds of consolidation for exactly that reason.

    Callers that want `<metadata_root>/socs/<vendor>/<family>/<part>.json`
    should keep using `resolve_soc_path()`; callers that root elsewhere, or
    want the slugs themselves, use this and root it their own way. Either
    way there is ONE place that knows a `silicon:` ref is three
    colon-separated parts.

    Note the one remaining independent encoding of that fact:
    `alp_cli/new_som.py::_SOC_REF_RE` (`^[a-z0-9-]+:[a-z0-9-]+:[a-z0-9-]+$`)
    validates the arity up front for its own `--soc-ref` flag, and would
    also need widening if the format ever grows a 4th part. It is a CLI
    input validator rather than a resolution site, so it is deliberately
    not folded in here -- but it is the other thing to change.
    """
    if not silicon:
        return None
    parts = silicon.split(":")
    if len(parts) != 3:
        return None
    return parts[0], parts[1], parts[2]


def resolve_soc_path(silicon: str | None, metadata_root: Path) -> Path | None:
    """Resolve a `vendor:family:part` `silicon:` key to the SoC-JSON path
    it names: `metadata/socs/<vendor>/<family>/<part>.json`.

    Returns None when `silicon` is falsy or not exactly 3 colon-separated
    parts -- does NOT check the path exists, callers decide what an
    unresolved/missing SoC spec means for them (e.g. a SoM preset with no
    `silicon:` at all is a valid, if incomplete, state; a `silicon:` that
    names a spec that isn't on disk is not).

    Single source for this resolution within `alp_project_loader.py` -- issue
    #997 collapsed the three copies that used to live here, in
    `resolve_capabilities()`, and inline in `pr-metadata-validate.yml` (now
    `check_som_topology_parity.py`) down to this one helper, and folded a
    fourth in-module copy from `resolve_memory_map()` into it too.

    Issue #1004 migrated the five sites that used to hand-roll this split
    (`gen_zephyr_board.py::_load_soc_spec` + its generated-file-banner
    string site, `validate_metadata.py`'s two soft-fail sites, and
    `alp_orchestrate/loader.py::_silicon_to_soc_path`) onto this helper,
    each behind a thin wrapper that preserves that site's original
    exception type or soft-fail shape.

    Issue #1096 closed out the remaining hand-rolled copies. The two that
    root at a metadata root now call this helper (`alp_model/targets.py`,
    `alp_cli/new_som.py`'s preset-path site); the rest root elsewhere and
    call `split_silicon_ref()` directly, including the three slug-extraction
    sites in `alp_cli/new_som.py` that #1096 originally scoped out -- they
    were the same three-part split, so leaving them would have left the
    drift the issue exists to close.

    There is no remaining `silicon.split(":")` outside this module; a
    regression test pins that.
    """
    parts = split_silicon_ref(silicon)
    if parts is None:
        return None
    vendor, family, part = parts
    return metadata_root / "socs" / vendor / family / f"{part}.json"


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
    soc_path = resolve_soc_path(sku_preset.get("silicon"), metadata_root)
    if soc_path is None or not soc_path.is_file():
        return None
    soc_spec = json.loads(soc_path.read_text(encoding="utf-8"))
    variants = soc_spec.get("variants") or []

    declared = sku_preset.get("silicon_variant")
    if declared and not is_tbd(declared):
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

    Raises ``alp_orchestrate.models.SdkRevisionUnknown`` when ``hw_rev`` is
    set but isn't a key in the family table.  Before #1025 an unrecognised
    ``hw_rev`` fell through to ``{}`` here, so the composed table silently
    emitted with base (production) pad routing instead of naming the
    wrong-hardware problem -- this is the site that bug lived at, and this
    emit path resolves its own SoM/board data independently of
    ``alp_orchestrate.loader.load_board_yaml``, so it needs its own gate.

    Also raises ``alp_orchestrate.models.SdkRevisionNotBuildable`` when
    ``hw_rev`` exists but its declared ``status:`` refuses a build
    (``reserved``, ``tbd``, or no ``status`` key at all -- #1025's broad
    reading).  Same rationale as the existence gate above: this emit path
    is its own independent resolution, so the status gate needs its own
    copy here too, not just in ``load_board_yaml``.

    Reads the table through ``sdk_compat.load_family_table()`` rather than
    its own ``yaml.safe_load``, so both independent readers of this file
    agree about what a damaged one means (#563).  This site used to do
    ``yaml.safe_load(...) or {}``, and that ``or {}`` FAILED OPEN on the
    two shapes that parse without raising: an EMPTY file and a file
    TRUNCATED above its ``hw_revisions:`` block both yielded ``{}``, both
    gates above then read "nothing to judge", and ``--emit
    composed-route-table`` shipped a wrong-hardware artefact at exit 0 for
    an hw_rev that does not exist -- the exact outcome the two gates below
    exist to prevent.  An unparseable table escaped as a raw
    ``yaml.ScannerError`` traceback: a refusal, but not a diagnosable one.
    The shared reader turns all of those into one coded
    ``OrchestratorError`` naming the file, which ``scripts/alp_project.py``
    already catches on this emit arm and reports at exit 1.  An ABSENT
    table still returns ``{}`` there and stays benign here.
    """
    if not hw_rev:
        return []
    try:
        family = _sku_family(sku)
    except ValueError:
        return []

    # Lazy import: alp_orchestrate imports this module at load time (the
    # resolve_memory_map edge -- see `_load_yaml` above), so the reverse
    # import must happen at call time, not at module scope.
    from alp_orchestrate.models import (SdkRevisionNotBuildable,
                                        SdkRevisionUnknown)
    from alp_orchestrate.sdk_compat import (load_family_table,
                                            revision_buildable,
                                            revision_known)
    data = load_family_table(metadata_root, family)
    if not data:
        # Absent table only: `load_family_table` raises on every
        # present-but-unusable shape, so reaching here with a falsy
        # `data` means the family genuinely ships no table.
        return []
    if revision_known(data, hw_rev) is False:
        available = sorted((data.get("hw_revisions") or {}).keys())
        raise SdkRevisionUnknown(
            f"SoM {sku} hw_rev {hw_rev!r} is not a known hardware "
            f"revision. Available hw_rev(s) for {sku}: {available}.")

    if revision_buildable(data, hw_rev) is False:
        status = (data.get("hw_revisions") or {}).get(hw_rev, {}).get("status")
        status_repr = f"status: {status!r}" if status is not None else \
            "carries no `status:` key"
        raise SdkRevisionNotBuildable(
            f"SoM {sku} hw_rev {hw_rev!r} exists but is not buildable "
            f"({status_repr}).")

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
    soc_path = resolve_soc_path(sku_preset.get("silicon"), metadata_root)
    if soc_path is None or not soc_path.is_file():
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
    soc_path = resolve_soc_path(sku_preset.get("silicon"), metadata_root)
    soc_caps: dict[str, Any] = {}
    if soc_path is not None and soc_path.is_file():
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
