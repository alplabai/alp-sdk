#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Validate every metadata/socs/**/*.json against the soc-spec v1
schema, every metadata/e1m_modules/<SKU>.yaml against the
som-preset v1 schema, and every metadata/boards/<name>.yaml
against the shared board-preset schema.

Run locally before pushing:

    python3 scripts/validate_metadata.py

CI invokes this from .github/workflows/pr-metadata-validate.yml on
every PR that touches metadata/.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import jsonschema

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("validate_metadata: PyYAML is required.  Install via `pip install pyyaml`.")

REPO = Path(__file__).resolve().parent.parent
# Needed so `alp_orchestrate.sdk_compat` resolves against THIS checkout's
# scripts/ even when an editable pip install (e.g. alp_sdk_cli) has
# registered a meta-path finder that would otherwise redirect the package
# import to a different alp-sdk checkout -- same idiom check_build_plan.py /
# check_system_manifest.py / check_emit_snapshots.py already use.
sys.path.insert(0, str(REPO / "scripts"))

from alp_project_loader import _sku_family, resolve_soc_path  # noqa: E402
from alp_orchestrate.sdk_compat import assert_exclusion_still_not_buildable  # noqa: E402
from strict_loaders import strict_json_loads, strict_yaml_load  # noqa: E402

# Power/ground nets are allowed as pin signals without a signals[] entry.
_POWER_NETS = {"VDD", "VDDIO", "VCC", "GND", "VSS", "AVDD", "DVDD"}


def _as_list(value) -> list:
    """Normalise a schema-typed array field to a list, tolerating a
    non-list value (e.g. an errant scalar/mapping in a malformed YAML/JSON
    manifest) instead of raising `TypeError` on iteration.

    JSON Schema validation is supposed to reject the shape, but every
    semantic pass below runs whether or not that pass already ran on this
    file -- and, for a file with no matching schema at all (a registry
    whose schema file is absent), it may never run.  Degrade to `[]`
    rather than let a bare scalar (`npus: 5`, `variants: 5`, ...) abort
    the whole gate mid-run with a traceback instead of a clean FAIL.
    """
    return value if isinstance(value, list) else []


def _as_dict(value) -> dict:
    """Normalise a schema-typed object field to a dict, tolerating a
    non-dict value instead of raising on `.get()`/`.items()`/`.keys()`.
    See `_as_list()` for why this runs regardless of schema-pass order.
    """
    return value if isinstance(value, dict) else {}


def _dict_entries(value) -> list[dict]:
    """`_as_list(value)` filtered to its dict entries -- the "array of
    schema-typed objects" shape used throughout this file (`npus[]`,
    `cores[]`, `variants[]`, `pins[]`, `realizations[]`, ...).  Combines
    the container-level guard (`_as_list`) with the existing per-entry
    `isinstance(x, dict)` filter so neither a non-list container nor a
    non-object entry can reach a bare `.get()`/`[...]` downstream.
    """
    return [v for v in _as_list(value) if isinstance(v, dict)]


SCHEMA = REPO / "metadata" / "schemas" / "soc-spec-v1.schema.json"
SOM_SCHEMA = REPO / "metadata" / "schemas" / "som-preset-v1.schema.json"
HWREV_SCHEMA = REPO / "metadata" / "schemas" / "hw-revisions-v1.schema.json"
SILICON_KCONFIG_SCHEMA = REPO / "metadata" / "schemas" / "silicon-kconfig-v1.schema.json"
SILICON_KCONFIG_REGISTRY = REPO / "metadata" / "registries" / "silicon-kconfig.json"
PERIPHERAL_KCONFIG_SCHEMA = REPO / "metadata" / "schemas" / "peripheral-kconfig-v1.schema.json"
PERIPHERAL_KCONFIG_REGISTRY = REPO / "metadata" / "registries" / "peripheral-kconfig.json"
TIER_A_LIBRARY_CI_SCHEMA = REPO / "metadata" / "schemas" / "tier-a-library-ci-v1.schema.json"
TIER_A_LIBRARY_CI_REGISTRY = REPO / "metadata" / "registries" / "tier-a-library-ci.json"
BOARD_PRESET_SCHEMA = REPO / "metadata" / "schemas" / "board-preset.schema.json"
LIBRARY_SCHEMA = REPO / "metadata" / "schemas" / "library-v1.schema.json"
SOC_SPEC_SCHEMA = REPO / "metadata" / "schemas" / "soc-spec-v1.schema.json"
SOCS = REPO / "metadata" / "socs"
SOM_PRESETS = REPO / "metadata" / "e1m_modules"
BOARD_PRESETS = REPO / "metadata" / "boards"
LIBRARIES = REPO / "metadata" / "libraries"
CHIP_SCHEMA = REPO / "metadata" / "schemas" / "chip-v1.schema.json"
CHIPS = REPO / "metadata" / "chips"
BLOCK_SCHEMA = REPO / "metadata" / "schemas" / "block-v1.schema.json"
BLOCKS = REPO / "metadata" / "blocks"
# Generated Zephyr board trees (one dir per <board>; each carries a twister
# .yaml whose `identifier:` is the fully-qualified <board>/<soc>/<cpucluster>
# triple `west build -b` resolves).  Ground truth for the board-target check.
ZEPHYR_ALP_BOARDS = REPO / "zephyr" / "boards" / "alp"


def _capability_vocabulary() -> set[str]:
    """The authoritative SoC capability key set (ADR 0018 `requires.capabilities`).

    Sourced the same way gen_soc_caps.py grounds its cap layer: the fixed
    `capabilities` property names in soc-spec-v1.schema.json (that object is
    `additionalProperties: false`, so its keys ARE the vocabulary).  A library
    manifest may only require a capability the SoC layer can actually resolve.
    """
    if not SOC_SPEC_SCHEMA.is_file():
        return set()
    schema = json.loads(SOC_SPEC_SCHEMA.read_text(encoding="utf-8"))
    caps = (schema.get("properties", {})
            .get("capabilities", {})
            .get("properties", {}))
    return set(caps.keys())


def _emit_pending_warnings(rel: Path, doc) -> None:
    """Non-fatal TODO surfaces for SoC JSONs that declare known-incomplete fields.

    Currently surfaces:

    * pending_reference_manual_ingestion -- peripherals: {} on such SoCs means
      "unknown / TBD", so ALP_SOC_*_COUNT ceilings on derived SoMs will
      under-report until the RM has been ingested.
    * peripherals_unverified (#936) -- a subset of `peripherals` keys this
      file itself flags as uncited (e.g. `pdm`/`pdm_lp`, uniform across every
      Alif Ensemble part with no datasheet/DFP citation).  Also catches a
      typo'd key that doesn't match anything in `peripherals`.
    """
    if not isinstance(doc, dict):
        return
    if doc.get("pending_reference_manual_ingestion"):
        print(f"WARN  {rel}: pending_reference_manual_ingestion -> "
              f"peripheral counts default to zero, ALP_SOC_*_COUNT ceilings "
              f"may under-report")
    unverified = doc.get("peripherals_unverified")
    if isinstance(unverified, list) and unverified:
        peripherals = doc.get("peripherals") if isinstance(doc.get("peripherals"), dict) else {}
        unknown = [k for k in unverified if k not in peripherals]
        if unknown:
            print(f"WARN  {rel}: peripherals_unverified references key(s) not present in "
                  f"peripherals: {unknown} -- likely a typo")
        print(f"WARN  {rel}: peripherals_unverified -> {sorted(unverified)} counts have no "
              f"datasheet/DFP/HWRM citation; the matching ALP_SOC_*_COUNT macros are asserted, "
              f"not confirmed")


def _check_files(label, files, validator, loader, key_for_summary):
    failures: list[tuple[Path, list[str]]] = []
    for path in files:
        rel = path.relative_to(REPO).as_posix()
        try:
            doc = loader(path)
        except Exception as e:
            failures.append((rel, [f"invalid {label} parse: {e}"]))
            print(f"FAIL {rel}: parse error ({e})")
            continue

        errors = sorted(validator.iter_errors(doc), key=lambda e: list(e.absolute_path))
        if errors:
            msgs = [
                f"{'/'.join(str(p) for p in err.absolute_path) or '<root>'}: {err.message}"
                for err in errors
            ]
            failures.append((rel, msgs))
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
        else:
            summary = doc.get(key_for_summary, "?") if isinstance(doc, dict) else "?"
            print(f"OK   {rel}  ({key_for_summary}={summary})")
            _emit_pending_warnings(rel, doc)
    return failures


def _check_silicon_capability_restrictions(som_files) -> list:
    """Cross-check SoM `silicon_capabilities.unpopulated` against the SoC JSON.

    The field is a RESTRICTION: a SKU may only mark unpopulated what the
    referenced silicon's `capabilities:` block actually offers (truthy value),
    and a name must not simultaneously appear in the preset's additive
    `capabilities:` block (that would make the merged value ambiguous).
    Returns a failure list shaped like _check_files().  Presets without the
    field are skipped -- absence means "full silicon capability set".
    """
    failures: list[tuple[Path, list[str]]] = []
    for path in som_files:
        rel = path.relative_to(REPO).as_posix()
        try:
            doc = strict_yaml_load(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # parse errors already reported by the schema pass
        if not isinstance(doc, dict):
            continue
        block = doc.get("silicon_capabilities")
        if not isinstance(block, dict):
            continue
        unpopulated = block.get("unpopulated") or []
        if not isinstance(unpopulated, list):
            continue  # wrong shape -- already failed the schema pass above

        msgs: list[str] = []
        soc_caps: dict = {}
        have_soc_caps = False
        silicon = str(doc.get("silicon", ""))
        soc_path = resolve_soc_path(silicon, SOCS.parent)
        if soc_path is None or not soc_path.is_file():
            msgs.append(f"silicon_capabilities: silicon ref `{silicon}` does not "
                        f"resolve to a metadata/socs/ spec, cannot validate "
                        f"`unpopulated:` against the silicon capability set")
        else:
            # A bare `json.loads` here used to raise `JSONDecodeError`
            # straight out of the gate on a syntactically invalid SoC
            # file -- the schema pass over `soc_files` reports THAT
            # failure separately; this cross-check only needs to
            # degrade gracefully when it can't read the referenced doc.
            try:
                soc_doc = json.loads(soc_path.read_text(encoding="utf-8"))
            except Exception as e:
                msgs.append(
                    f"silicon_capabilities: silicon ref `{silicon}` resolves to "
                    f"{soc_path.relative_to(REPO).as_posix()} but it fails to "
                    f"parse ({e}), cannot validate `unpopulated:` against the "
                    f"silicon capability set")
            else:
                # `capabilities:` is schema-typed as an object, but a
                # malformed SoC doc (or a non-dict top level entirely)
                # can carry a scalar there -- `soc_caps.get(name)` /
                # `.items()` below would raise on that. Normalise to `{}`
                # rather than crash the gate.
                soc_caps = _as_dict(soc_doc.get("capabilities") if isinstance(soc_doc, dict) else None)
                have_soc_caps = True

        # Same reasoning for the preset's own `capabilities:` block.
        som_caps = _as_dict(doc.get("capabilities"))
        for name in unpopulated:
            if not isinstance(name, str):
                # A non-string entry (e.g. a nested dict) is already a
                # schema-shape violation reported by the schema pass; used
                # unfiltered it would also raise `TypeError: unhashable
                # type` on `soc_caps.get(name)` / `name in som_caps` below.
                continue
            if have_soc_caps and not soc_caps.get(name):
                offered = ", ".join(sorted(k for k, v in soc_caps.items() if v)) or "<none>"
                msgs.append(
                    f"silicon_capabilities/unpopulated[{name}]: not a capability the "
                    f"referenced silicon `{silicon}` offers -- a SKU can only remove "
                    f"what the SoC JSON `capabilities:` block declares truthy "
                    f"(offered: {offered})")
            if name in som_caps:
                msgs.append(
                    f"silicon_capabilities/unpopulated[{name}]: also declared in this "
                    f"preset's `capabilities:` block -- a capability is either "
                    f"SoM-added or silicon-unpopulated, never both")

        if msgs:
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
            failures.append((rel, msgs))
        else:
            print(f"OK   {rel}  (silicon_capabilities: {len(unpopulated)} "
                  f"unpopulated cap(s) resolve against {silicon})")
    return failures


def _check_som_peripheral_instance_uniqueness(som_files) -> list:
    """Reject duplicate `instance` slugs within one preset's `soc_peripheral_instances`.

    The schema has no per-key uniqueness constraint for this array -- JSON
    Schema `uniqueItems` only rejects two byte-identical objects, so two
    entries naming the same `instance` slug with a different `class` /
    `driver_status` validate cleanly today.  #655's DT/pinctrl generation
    binds nodes by this slug, so a duplicate is a real generation hazard,
    not an editorial nit.  Returns a failure list shaped like
    `_check_files()`.  Presets without the block are skipped.
    """
    failures: list[tuple[Path, list[str]]] = []
    for path in som_files:
        rel = path.relative_to(REPO).as_posix()
        try:
            doc = strict_yaml_load(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # parse errors already reported by the schema pass
        if not isinstance(doc, dict):
            continue
        entries = doc.get("soc_peripheral_instances")
        if not isinstance(entries, list):
            continue

        counts: dict[str, int] = {}
        for entry in entries:
            if not isinstance(entry, dict):
                continue
            inst = entry.get("instance")
            if isinstance(inst, str):
                counts[inst] = counts.get(inst, 0) + 1

        msgs = [
            f"soc_peripheral_instances: instance `{inst}` declared {count} "
            f"times -- slugs must be unique within a preset (#655 binds DT "
            f"nodes by this name)"
            for inst, count in sorted(counts.items()) if count > 1
        ]

        if msgs:
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
            failures.append((rel, msgs))
        else:
            print(f"OK   {rel}  (soc_peripheral_instances: {len(entries)} "
                  f"unique instance slug(s))")
    return failures


def _check_som_slot0_address_resolved(som_files) -> list:
    """Refuse a `memory_map:` region that names an MRAM slot0 path but
    carries no resolved address (tan-cli#353).

    `scripts/alp_orchestrate/loader.py::_resolve_slot0_load_address` (and
    `scripts/gen_zephyr_board.py::_aen_role_slot0_map` for board
    generation) both key an AEN core's slot0-XIP load address off a
    `memory_map:` region literally NAMED `<role>_slot0` (`he_slot0` /
    `hp_slot0`) with an integer `base:`. A region that spells the name --
    declaring the slot0 PATH exists -- but leaves `base:` as `"TBD"`,
    absent, or any other non-integer silently falls through both readers:
    the loader treats it as "no override" (picking the wrong default, or
    None for `hp`) and the generator's `_aen_flash_partitions` raises only
    at BUILD time, deep inside DTS emission, with no metadata-level
    signal. Catch it here instead, at the one place authoring a SoM
    preset already gets feedback.

    JSON Schema can express that `base:` is `integer | "TBD"`
    (`metadata/schemas/som-preset-v1.schema.json`'s `memory_region`) but
    not "declares itself a `*_slot0` region, so `base` may not be the
    `TBD` half of that union" -- that's a semantic rule over the region's
    OWN `name:`, not a shape constraint. Returns a failure list shaped
    like `_check_files()`. Presets with no `memory_map:` are skipped.
    """
    failures: list[tuple[Path, list[str]]] = []
    for path in som_files:
        rel = path.relative_to(REPO).as_posix()
        try:
            doc = strict_yaml_load(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # parse errors already reported by the schema pass
        if not isinstance(doc, dict):
            continue
        memory_map = doc.get("memory_map")
        if not isinstance(memory_map, list):
            continue

        msgs: list[str] = []
        slot0_regions = 0
        for region in memory_map:
            if not isinstance(region, dict):
                continue
            name = region.get("name")
            if not isinstance(name, str) or not name.endswith("_slot0"):
                continue
            slot0_regions += 1
            if not isinstance(region.get("base"), int):
                msgs.append(
                    f"memory_map: region `{name}` declares an MRAM slot0 "
                    f"path but its `base` ({region.get('base')!r}) is not "
                    f"a resolved address -- tan's Flow D "
                    f"flash_args.slot0_load_address needs a concrete "
                    f"integer, not a TBD/missing placeholder")

        if msgs:
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
            failures.append((rel, msgs))
        elif slot0_regions:
            print(f"OK   {rel}  (memory_map: {slot0_regions} slot0 "
                  f"region(s) all resolve a concrete base address)")
    return failures


def _check_silicon_kconfig() -> list:
    """Validate the silicon->Kconfig registry and its socs/ correspondence.

    Schema-checks metadata/registries/silicon-kconfig.json, then asserts
    every `knownSilicon` ref resolves to an existing metadata/socs/ spec
    (the registry is the Kconfig allowlist; the SoC tree is the fact).
    Returns a failure list shaped like _check_files().
    """
    failures: list[tuple[Path, list[str]]] = []
    if not SILICON_KCONFIG_REGISTRY.is_file():
        return failures  # optional gate; skip when absent
    rel = SILICON_KCONFIG_REGISTRY.relative_to(REPO).as_posix()
    try:
        data = json.loads(SILICON_KCONFIG_REGISTRY.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"FAIL {rel}: parse error ({e})")
        return [(rel, [f"invalid JSON parse: {e}"])]

    # `data.get("knownSilicon", [])` below ran unconditionally, before this
    # guard existed, regardless of whether the schema pass below already
    # flagged a non-object top level -- a registry parsing to a bare JSON
    # list (or any other non-dict) reached `data.get(...)` and raised
    # `AttributeError`, aborting the gate mid-run instead of reporting the
    # schema FAIL line that already names the real problem.
    if not isinstance(data, dict):
        msg = f"top-level value is a {type(data).__name__}, expected an object"
        print(f"FAIL {rel}: {msg}")
        return [(rel, [msg])]

    msgs: list[str] = []
    if SILICON_KCONFIG_SCHEMA.is_file():
        schema = json.loads(SILICON_KCONFIG_SCHEMA.read_text(encoding="utf-8"))
        validator = jsonschema.Draft202012Validator(schema)
        for err in sorted(validator.iter_errors(data), key=lambda e: list(e.absolute_path)):
            loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
            msgs.append(f"{loc}: {err.message}")

    for ref in _as_list(data.get("knownSilicon")):
        if ref and not isinstance(ref, str):
            # A TRUTHY non-string entry (e.g. a nested dict/int) is already
            # a schema-shape violation reported by the schema pass; used
            # unfiltered it would also raise `AttributeError` inside
            # `resolve_soc_path()` -> `split_silicon_ref()`'s colon split
            # on a non-string value. A FALSY entry (JSON `null`, `0`, `""`)
            # is deliberately let through instead of skipped here --
            # `split_silicon_ref()`'s own falsy check already handles
            # every falsy value safely, and a `null` entry is pinned
            # elsewhere to still report the "not a ref" message rather
            # than being silently skipped.
            continue
        soc_path = resolve_soc_path(ref, SOCS.parent)
        if soc_path is None:
            msgs.append(f"knownSilicon[{ref}]: not a <vendor>:<family>:<part> ref")
            continue
        if not soc_path.is_file():
            msgs.append(f"knownSilicon[{ref}]: no SoC spec at "
                        f"{soc_path.relative_to(REPO).as_posix()}")

    if msgs:
        print(f"FAIL {rel}")
        for m in msgs:
            print(f"  · {m}")
        failures.append((rel, msgs))
    else:
        n = len(_as_list(data.get("knownSilicon")))
        print(f"OK   {rel}  (knownSilicon={n}, all resolve to socs/)")
    return failures


def _check_peripheral_kconfig() -> list:
    """Validate the peripheral-token -> Zephyr Kconfig registry."""
    failures: list[tuple[Path, list[str]]] = []
    if not PERIPHERAL_KCONFIG_REGISTRY.is_file():
        return failures
    rel = PERIPHERAL_KCONFIG_REGISTRY.relative_to(REPO).as_posix()
    try:
        data = json.loads(PERIPHERAL_KCONFIG_REGISTRY.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"FAIL {rel}: parse error ({e})")
        return [(rel, [f"invalid JSON parse: {e}"])]

    # `data.get("peripherals", {})` below is reached only when `msgs` stays
    # empty, and today it stays safe only BY ACCIDENT: when
    # PERIPHERAL_KCONFIG_SCHEMA exists, a non-dict `data` fails the object
    # type check and lands in `msgs`, skipping the `else` branch below --
    # but that's incidental to the schema pass running at all, not a
    # guarantee. Make it deliberate (same shape as _check_silicon_kconfig).
    #
    # Reachability, stated honestly: on the real CLI path this guard
    # cannot fire against a malformed ON-DISK registry. Importing this
    # module already transitively imports `alp_orchestrate`, which calls
    # `alp_registries.peripheral_kconfig()` at MODULE scope
    # (`alp_orchestrate/slugs.py`) against the SAME
    # PERIPHERAL_KCONFIG_REGISTRY file, before `main()` -- and this
    # function -- ever run. A malformed registry now raises there first
    # (a `ValueError`, not a crash), aborting `import validate_metadata`
    # itself. Kept anyway, deliberately, because it IS reachable when
    # this function runs against a registry path re-pointed after a
    # successful import -- every regression test for this function does
    # exactly that -- and as defence in depth should the import-time
    # guard's shape ever change.
    if not isinstance(data, dict):
        msg = f"top-level value is a {type(data).__name__}, expected an object"
        print(f"FAIL {rel}: {msg}")
        return [(rel, [msg])]

    msgs: list[str] = []
    if PERIPHERAL_KCONFIG_SCHEMA.is_file():
        schema = json.loads(PERIPHERAL_KCONFIG_SCHEMA.read_text(encoding="utf-8"))
        validator = jsonschema.Draft202012Validator(schema)
        for err in sorted(validator.iter_errors(data), key=lambda e: list(e.absolute_path)):
            loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
            msgs.append(f"{loc}: {err.message}")

    if msgs:
        print(f"FAIL {rel}")
        for m in msgs:
            print(f"  · {m}")
        failures.append((rel, msgs))
    else:
        n = len(_as_dict(data.get("peripherals")))
        print(f"OK   {rel}  (peripherals={n})")
    return failures


def _check_chip_semantics(chip_files) -> list:
    """Cross-check beyond pure schema validation: `chip_id:` matches filename.

    Mirrors `_check_library_semantics()`'s `name == path.stem` check: the
    `chip_id` a board/SoM manifest references must resolve by filename, so a
    mismatch (copy-paste drift between `metadata/chips/<part>.yaml` and its
    `chip_id:` field) would silently break that lookup.  Returns a failure
    list shaped like `_check_files()`.
    """
    failures: list[tuple[Path, list[str]]] = []
    for path in chip_files:
        rel = path.relative_to(REPO).as_posix()
        try:
            doc = strict_yaml_load(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # parse errors already reported by the schema pass
        if not isinstance(doc, dict):
            continue

        msgs: list[str] = []

        chip_id = doc.get("chip_id")
        if isinstance(chip_id, str) and chip_id != path.stem:
            msgs.append(
                f"chip_id: `{chip_id}` must match the manifest filename `{path.stem}` "
                f"-- chip_id lookups resolve by filename")

        if msgs:
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
            failures.append((rel, msgs))
    return failures


def _check_soc_npu_pairing(soc_files) -> list:
    """Cross-ref the SoC `npus[].paired_core` field against `cores[]`.

    `paired_core` is the single source of truth for which CPU core drives an
    NPU instance (the build emit sizes the accelerator per target core from
    it -- scripts/alp_orchestrate/kconfig.py); JSON Schema cannot express the
    cross-reference, so enforce it here:

      1. every `npus[].paired_core` must name a real `cores[].id` in the same
         SoC JSON (a typo would silently disable the per-core sizing);
      2. when one NPU `type` appears with more than one distinct
         `mac_per_cycle` (e.g. the Alif E3/E5/E7's 256-MAC + 128-MAC U55s),
         every instance of that type MUST declare `paired_core` -- otherwise
         the emit cannot tell the cores apart and a 256-MAC stream would error
         a 128-MAC NPU at invoke (issue #909).

    A single-MAC variant, or an instance on a shared non-core subsystem (the
    E8 U85 on the HG subsystem), legitimately omits `paired_core`.
    Returns a failure list shaped like `_check_files()`.
    """
    failures: list[tuple[Path, list[str]]] = []
    for path in soc_files:
        try:
            doc = strict_json_loads(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # parse errors already reported by the schema pass
        if not isinstance(doc, dict):
            continue  # non-object top level; schema pass already flags it
        # `npus[]`/`cores[]` may themselves be a non-list scalar, and their
        # entries are schema-typed objects -- but the schema pass that
        # would reject either malformation is not guaranteed to have run
        # first. `_dict_entries()` filters to dicts rather than let a
        # non-list container or a non-object entry raise `AttributeError`/
        # `TypeError` here and abort the whole gate mid-run, hiding the
        # schema FAIL line that already explains the real problem (same
        # shape as `_check_chip_physical`).
        npus = _dict_entries(doc.get("npus"))
        if not npus:
            continue
        rel = path.relative_to(REPO).as_posix()
        # `c.get("id")` is schema-typed as a string, but a malformed SoC
        # doc can carry any value there -- an unfiltered set comprehension
        # raises `TypeError: unhashable type` building this set from a
        # dict/list `id`, and a mixed str/int `id` set raises on the
        # `sorted()` call below. Filter to strings, same idiom as the
        # `unpopulated` guard in `_check_silicon_capability_restrictions()`.
        core_ids = {
            c.get("id") for c in _dict_entries(doc.get("cores"))
            if isinstance(c.get("id"), str)
        }
        msgs: list[str] = []

        # (1) referential integrity of every declared paired_core.
        for i, n in enumerate(npus):
            pc = n.get("paired_core")
            # `pc not in core_ids` alone raises `TypeError: unhashable
            # type` when `pc` is a dict/list -- short-circuit on a
            # non-string `pc` first so a malformed value is reported as a
            # mismatch instead of aborting the gate.
            if pc is not None and (not isinstance(pc, str) or pc not in core_ids):
                msgs.append(
                    f"npus[{i}] ({n.get('type')}/{n.get('subtype')}): "
                    f"paired_core={pc!r} is not a cores[].id "
                    f"(known: {sorted(core_ids)})")

        # (2) multi-MAC variants must pair every instance to a core.
        by_type: dict[str, list[dict]] = {}
        for n in npus:
            by_type.setdefault(str(n.get("type", "")), []).append(n)
        for ntype, insts in by_type.items():
            # `mac_per_cycle` is schema-typed as an integer, but a
            # malformed doc can carry a dict/list there (unhashable --
            # `TypeError` building this set) or a str alongside a real
            # int (mixed-type `sorted()` below raises too). Filter to
            # ints, same idiom as `core_ids` above.
            macs = {
                n.get("mac_per_cycle") for n in insts
                if isinstance(n.get("mac_per_cycle"), int)
            }
            if len(macs) > 1:
                unpaired = [n for n in insts if not n.get("paired_core")]
                if unpaired:
                    subs = ", ".join(str(n.get("subtype")) for n in unpaired)
                    msgs.append(
                        f"{ntype} appears with distinct MAC arrays {sorted(macs)} "
                        f"but instance(s) [{subs}] omit paired_core -- the build "
                        f"cannot size the accelerator per core (see #909)")

        if msgs:
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
            failures.append((rel, msgs))
    return failures


def _check_soc_debug_probe_identity(soc_files) -> list:
    """Cross-ref `variants[].debug.jlink_device` keys against `cores[].id`,
    and require the `expect_dpidr`/`jlink_device` preflight PAIR to be whole.

    #987 publishes the debug-probe identity (J-Link device, pyOCD target)
    per variant, with `jlink_device` keyed by core id since a J-Link attach
    device can in principle differ per core class (a future non-M55 core
    would need a different value, even though every Alif M55 core today
    shares the generic `Cortex-M55` string).  JSON Schema can express that
    `jlink_device` is an object of string values but not that its *keys*
    are real cores on *this* SoC -- a typo (or a stale key surviving a core
    rename) would silently point the extension's launch-config generator at
    a core that does not exist.  Enforce it here.

    #1355 adds `expect_dpidr` -- the SW-DP IDR a host flasher compares
    against BEFORE any write, so it can abort on the wrong board while the
    session is still read-only.  That check needs TWO facts: the expected ID
    and the live-core attach profile (`jlink_device`) the read is performed
    with.  A variant publishing `expect_dpidr` without a `jlink_device`
    entry for every one of this SoC's cores is not merely half-documented --
    tan refuses a half-armed pair outright (`flash_plan.py::
    validate_flow_d_preflight_args`), so it would turn every flash of that
    part, dry runs included, into a hard error.  Schema cannot say "this key
    implies that one is complete across cores[]"; this can.  The converse is
    deliberately NOT an error: `jlink_device` alone is the correct state of
    every variant whose DPIDR nobody has measured yet, and leaving the guard
    unarmed is far safer than arming it at a guessed ID.

    Returns a failure list shaped like `_check_files()`.
    """
    failures: list[tuple[Path, list[str]]] = []
    for path in soc_files:
        try:
            doc = strict_json_loads(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # parse errors already reported by the schema pass
        if not isinstance(doc, dict):
            continue  # non-object top level; schema pass already flags it
        # `variants[]`/`cores[]` may themselves be a non-list scalar, and
        # their entries are schema-typed objects -- but the schema pass
        # that would reject either malformation is not guaranteed to have
        # run first. `_dict_entries()` filters to dicts rather than let a
        # non-list container or a non-object entry raise `AttributeError`/
        # `TypeError` here and abort the whole gate mid-run, hiding the
        # schema FAIL line that already explains the real problem (same
        # shape as `_check_chip_physical`).
        variants = _dict_entries(doc.get("variants"))
        if not variants:
            continue
        rel = path.relative_to(REPO).as_posix()
        # `c.get("id")` is schema-typed as a string, but a malformed SoC
        # doc can carry any value there -- an unfiltered set comprehension
        # raises `TypeError: unhashable type` building this set from a
        # dict/list `id`, and a mixed str/int `id` set raises on the
        # `sorted()` calls below (`core_id!r ... sorted(core_ids)` and the
        # `expect_dpidr` uncovered-core sort). Filter to strings, same
        # idiom as `_check_soc_npu_pairing()`'s `core_ids`.
        core_ids = {
            c.get("id") for c in _dict_entries(doc.get("cores"))
            if isinstance(c.get("id"), str)
        }
        # Cortex-M cores only for the `expect_dpidr` pairing rule below: the
        # DPIDR preflight guards the Zephyr-on-M J-Link flash path, and
        # `debug.jlink_device` is legitimately sparse across `cores[]` --
        # E8 publishes an attach profile for m55_hp/m55_he and none for
        # a32_cluster, an A-cluster that boots Linux off storage rather than
        # being J-Link flashed. Demanding coverage of every core would fail
        # the very variant this rule exists to protect.
        m_core_ids = {
            c.get("id") for c in _dict_entries(doc.get("cores"))
            if isinstance(c.get("id"), str) and str(c.get("type") or "").startswith("cortex-m")
        }
        msgs: list[str] = []

        for i, v in enumerate(variants):
            debug = v.get("debug")
            debug = debug if isinstance(debug, dict) else {}
            jlink_device = debug.get("jlink_device") or {}
            jlink_device = jlink_device if isinstance(jlink_device, dict) else {}
            for core_id in jlink_device:
                if core_id not in core_ids:
                    msgs.append(
                        f"variants[{i}] ({v.get('order_code')}): "
                        f"debug.jlink_device key {core_id!r} is not a "
                        f"cores[].id (known: {sorted(core_ids)})")

            if debug.get("expect_dpidr"):
                uncovered = sorted(m_core_ids - set(jlink_device))
                if uncovered:
                    msgs.append(
                        f"variants[{i}] ({v.get('order_code')}): "
                        f"debug.expect_dpidr is published but "
                        f"debug.jlink_device carries no attach profile for "
                        f"Cortex-M core(s) {uncovered} -- the wrong-board "
                        f"SW-DP IDR preflight needs both, and a half-armed "
                        f"pair is refused downstream rather than skipped "
                        f"(#1355)")

        if msgs:
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
            failures.append((rel, msgs))
    return failures


def _check_soc_jlink_flash_device_declared(soc_files) -> list:
    """Every Alif Ensemble variant must publish `debug.jlink_flash_device`,
    as a string or explicit `null` -- never omit the key.

    #1295: an absent key makes tan's `flow_d_available()` false, which
    SILENTLY downgrades Flow D (the J-Link MRAM loader) to the SE-UART
    Flow A path with no diagnostic -- the AEN runbook's #1 trap. JSON
    Schema can express "if present, string-or-null" but not "present on
    every variant of THIS vendor/family" (soc-spec-v1 also covers
    Renesas/NXP/DEEPX parts, where this field doesn't apply), so enforce
    the presence rule here, scoped to Alif Ensemble by `vendor` + `family`.

    A published `null` is the correct state for a genuinely unresolved
    device profile (e.g. e4's AE402FA0E5597LE0, which the SEGGER J-Link
    device DB does not carry under any spelling as of DLL V9.46) -- it
    converts the invisible Flow A downgrade into tan's loud refusal
    (`plan_alif_mram_jlink`) rather than a silent transport switch. Only a
    missing KEY fails this check; `null` and any non-empty string both pass.

    Returns a failure list shaped like `_check_files()`.
    """
    failures: list[tuple[Path, list[str]]] = []
    for path in soc_files:
        try:
            doc = strict_json_loads(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # parse errors already reported by the schema pass
        if not isinstance(doc, dict):
            continue  # non-object top level; schema pass already flags it
        if doc.get("vendor") != "Alif Semiconductor" or doc.get("family") != "Ensemble":
            continue
        # `variants[]` may itself be a non-list scalar, and its entries are
        # schema-typed objects -- but the schema pass that would reject
        # either malformation is not guaranteed to have run first.
        # `_dict_entries()` filters to dicts rather than let a non-list
        # container or a non-object entry raise `AttributeError`/`TypeError`
        # here and abort the whole gate mid-run, hiding the schema FAIL line
        # that already explains the real problem (same shape as
        # `_check_chip_physical`).
        variants = _dict_entries(doc.get("variants"))
        if not variants:
            continue
        rel = path.relative_to(REPO).as_posix()
        msgs: list[str] = []

        for i, v in enumerate(variants):
            debug = v.get("debug")
            debug = debug if isinstance(debug, dict) else {}
            if "jlink_flash_device" not in debug:
                msgs.append(
                    f"variants[{i}] ({v.get('order_code')}): "
                    f"debug.jlink_flash_device is absent -- every Alif "
                    f"Ensemble variant must publish either the device-profile "
                    f"string or explicit null (a declared known-unknown); an "
                    f"absent key silently downgrades Flow D to the SE-UART "
                    f"Flow A path with no diagnostic (#1295)")

        if msgs:
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
            failures.append((rel, msgs))
    return failures


def _check_soc_no_wlcsp_variants(soc_files) -> list:
    """No Alif Ensemble variant may declare a WLCSP package (#1444).

    Alp Lab modules are BGA only, so a WLCSP row is a part this corpus
    will never ship. That is a product decision, not a fact about the
    silicon -- Alif genuinely offers WLCSP208/WLCSP216 across several
    subfamilies, and each file's own `packages` list still says so.

    It is worth a gate rather than a note because the natural way to
    extend `variants[]` is to work down the vendor pack's device list,
    and the pack does not distinguish the packages we buy from the ones
    we do not. Nine WLCSP rows accumulated that way. Each carried a
    `debug` block -- a pyocd target and a J-Link flash profile -- for
    hardware that does not exist here, which is exactly the sort of
    never-exercised device string that later gets copied onto a part it
    does not belong to.

    Scoped to Alif Ensemble by `vendor` + `family`: soc-spec-v1 also
    covers Renesas/NXP/DEEPX parts, where this rule is not ours to make.
    Checks `variants[].package` only -- the file-level `packages` list is
    a vendor fact and must keep naming WLCSP.

    Returns a failure list shaped like `_check_files()`.
    """
    failures: list[tuple[Path, list[str]]] = []
    for path in soc_files:
        try:
            doc = strict_json_loads(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # parse errors already reported by the schema pass
        if not isinstance(doc, dict):
            continue  # non-object top level; schema pass already flags it
        if doc.get("vendor") != "Alif Semiconductor" or doc.get("family") != "Ensemble":
            continue
        rel = path.relative_to(REPO).as_posix()
        msgs: list[str] = []

        # `variants[]` may itself be a non-list scalar, and its entries are
        # schema-typed objects -- but the schema pass that would reject
        # either malformation is not guaranteed to have run first.
        # `_dict_entries()` filters to dicts rather than let a non-list
        # container or a non-object entry raise `AttributeError`/`TypeError`
        # here and abort the whole gate mid-run, hiding the schema FAIL line
        # that already explains the real problem (same shape as
        # `_check_chip_physical`).
        variants = _dict_entries(doc.get("variants"))
        for i, v in enumerate(variants):
            # `package` is schema-typed as a string, but a malformed
            # document can carry a non-string truthy value there (e.g. the
            # bare int `208`) -- `package.upper()` would raise
            # `AttributeError` on that. Normalise to a string first, same
            # shape as every other scalar guard in this file.
            package = v.get("package")
            package = package if isinstance(package, str) else ""
            if "WLCSP" in package.upper():
                msgs.append(
                    f"variants[{i}] ({v.get('order_code')}): package "
                    f"{package!r} is WLCSP -- Alp Lab modules are BGA only, "
                    f"so this corpus carries no WLCSP variant in any Alif "
                    f"Ensemble subfamily (#1444). Drop the variant; the "
                    f"file-level `packages` list is where the vendor's WLCSP "
                    f"offering stays recorded")

        if msgs:
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
            failures.append((rel, msgs))
    return failures


def _check_chip_physical(chip_files) -> list:
    """Semantic cross-checks for chip `physical:` block (pin/passive→signal resolution + pad uniqueness).

    Every `pins[].signal` must resolve to a declared `signals[]` name or a
    power/ground net, every `passives[].net` must resolve the same way, and a
    footprint pad must appear at most once.  Mirrors `_check_library_semantics()`:
    schema validates shape; this pass validates meaning.  Returns a failure
    list shaped like `_check_files()`.
    """
    failures: list = []
    for path in chip_files:
        try:
            rel = path.relative_to(REPO).as_posix()
        except ValueError:
            rel = path.as_posix()  # out-of-tree (e.g. a test fixture); report as-is
        try:
            doc = strict_yaml_load(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # parse errors already reported by the schema pass
        if not isinstance(doc, dict):
            continue
        phys = doc.get("physical")
        if not phys:
            continue
        if not isinstance(phys, dict):
            # `physical` is schema-typed as an object, but the schema pass
            # that would reject a non-object value (e.g. a bare string,
            # which passes the `if not phys` truthiness guard above when
            # non-empty) is not guaranteed to have run first -- skip rather
            # than let `phys.get(...)` raise `AttributeError` here and abort
            # the whole gate mid-run, hiding the schema FAIL line that
            # already explains the real problem (same shape as
            # `_check_board_targets`'s `topology` guard below).
            continue
        # `signals[]`/`pins[]`/`passives[]` may themselves be a non-list
        # scalar, and their entries are schema-typed objects -- but the
        # schema pass that would reject either malformation is not
        # guaranteed to have run first. `_dict_entries()` filters to dicts
        # rather than let a non-list container or a non-object entry raise
        # `AttributeError`/`TypeError` on `.get()` here (same shape as
        # `_check_soc_npu_pairing`).
        # `s.get("name")` is schema-typed as a string, but a malformed chip
        # manifest can carry a dict/list there -- an unfiltered set
        # comprehension raises `TypeError: unhashable type` building this
        # set. Filter to strings, same idiom as `core_ids` in
        # `_check_soc_npu_pairing()`.
        sig_names = {
            s["name"] for s in _dict_entries(doc.get("signals"))
            if isinstance(s.get("name"), str)
        }
        msgs: list = []
        seen_pads: dict = {}
        for pin in _dict_entries(phys.get("pins")):
            sig = pin.get("signal"); pad = pin.get("pad")
            # `sig`/`pad` are schema-typed strings, but a malformed
            # manifest can carry a dict/list there -- `sig not in
            # sig_names` / `pad in seen_pads` raise `TypeError: unhashable
            # type` unfiltered. Scope the skip to the actual hazard
            # (dict/list is unhashable) rather than a blanket
            # `not isinstance(..., str)`: every other schema-shape
            # violation (int, bool, YAML `null`) IS hashable and safe to
            # membership-test, and a blanket str-only filter would
            # silently drop the "not in signals[]" / "used more than
            # once" diagnostics for those values instead of reporting
            # them.
            if isinstance(sig, (dict, list)) or (sig not in sig_names and sig not in _POWER_NETS):
                msgs.append(f"physical.pins pad {pad}: signal '{sig}' not in signals[] or power nets")
            if not isinstance(pad, (dict, list)):
                if pad in seen_pads:
                    msgs.append(f"physical.pins: pad '{pad}' used more than once")
                seen_pads[pad] = True
        for passive in _dict_entries(phys.get("passives")):
            net = passive.get("net")
            # Same reasoning as `sig` above -- guard the unhashable case
            # before the set-membership tests.
            if isinstance(net, (dict, list)) or (net not in sig_names and net not in _POWER_NETS):
                msgs.append(f"physical.passives: net '{net}' not in signals[] or power nets")
        if msgs:
            failures.append((rel, msgs))
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
    return failures


def _check_block_realizations(block_files, chip_files) -> list:
    """Semantic cross-checks for block `realizations[].parts[].chip`, `maps`, and `passives[].net`.

    Every `realizations[].parts[].chip` must resolve to a chip manifest filename,
    every `maps` value must name a signal declared in the block's `interface`,
    and every `realizations[].passives[].net` must resolve to an `interface`
    signal or a power/ground net.  Returns a failure list shaped like
    `_check_files()`.
    """
    failures: list = []
    chip_ids = {p.stem for p in chip_files}
    for path in block_files:
        try:
            rel = path.relative_to(REPO).as_posix()
        except ValueError:
            rel = path.as_posix()  # out-of-tree (e.g. a test fixture); report as-is
        try:
            doc = strict_yaml_load(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # parse errors already reported by the schema pass
        if not isinstance(doc, dict):
            continue
        # `e.get("signal")` is schema-typed as a string, but a malformed
        # block manifest can carry a dict/list there -- an unfiltered set
        # comprehension raises `TypeError: unhashable type` building this
        # set. Filter to strings, same idiom as `sig_names` in
        # `_check_chip_physical()`.
        iface = {
            e["signal"] for e in _dict_entries(doc.get("interface"))
            if isinstance(e.get("signal"), str)
        }
        msgs: list = []
        # `realizations[]`/`parts[]`/`passives[]` may themselves be a
        # non-list scalar, and their entries are schema-typed objects -- but
        # the schema pass that would reject either malformation is not
        # guaranteed to have run first. `_dict_entries()` filters to dicts
        # rather than let a non-list container or a non-object entry raise
        # `AttributeError`/`TypeError` on `.get()` here (same shape as
        # `_check_soc_npu_pairing`).
        for r in _dict_entries(doc.get("realizations")):
            for part in _dict_entries(r.get("parts")):
                # `chip` is schema-typed as a string, but a malformed
                # manifest can carry a dict/list there -- `not in
                # chip_ids` raises `TypeError: unhashable type`
                # unfiltered. Guard before the membership test.
                chip = part.get("chip")
                if not isinstance(chip, str) or chip not in chip_ids:
                    msgs.append(f"realization '{r.get('id')}': part chip '{chip}' has no metadata/chips manifest")
                maps = _as_dict(part.get("maps"))
                for _pin, sig in maps.items():
                    # Same reasoning -- a `maps` value can be any YAML
                    # type; `sig not in iface` raises unfiltered.
                    if not isinstance(sig, str) or sig not in iface:
                        msgs.append(f"realization '{r.get('id')}': maps target '{sig}' not in interface[]")
            for passive in _dict_entries(r.get("passives")):
                net = passive.get("net")
                # Same reasoning as `chip`/`sig` above.
                if not isinstance(net, str) or (net not in iface and net not in _POWER_NETS):
                    msgs.append(f"realization '{r.get('id')}': passives net '{net}' not in interface[] or power nets")
        if msgs:
            failures.append((rel, msgs))
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
    return failures


def _check_library_semantics(library_files) -> list:
    """Cross-checks on library manifests beyond pure schema validation (ADR 0018).

    Schema already enforces the licence allowlist and the tier/os enums; this
    pass adds the two facts the schema cannot express:

      * every `requires.capabilities` key names a real SoC capability
        (validated against `_capability_vocabulary()`), so an incompatible
        selection is rejected early and clearly rather than emitting a dead
        Kconfig line; and
      * `name:` matches the manifest filename (`<name>.yaml`), so the
        `libraries: [<name>]` token a project writes always resolves.

    Returns a failure list shaped like _check_files().
    """
    failures: list[tuple[Path, list[str]]] = []
    vocab = _capability_vocabulary()
    for path in library_files:
        try:
            rel = path.relative_to(REPO).as_posix()
        except ValueError:
            rel = path.as_posix()  # out-of-tree (e.g. a test fixture); report as-is
        try:
            doc = strict_yaml_load(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # parse / schema errors already reported by the schema pass
        if not isinstance(doc, dict):
            continue

        msgs: list[str] = []

        name = doc.get("name")
        if isinstance(name, str) and name != path.stem:
            msgs.append(
                f"name: `{name}` must match the manifest filename `{path.stem}` "
                f"-- the `libraries: [{path.stem}]` token resolves by filename")

        requires = doc.get("requires") or {}
        if isinstance(requires, dict):
            # `capabilities` may itself be a non-list scalar (e.g. a bare
            # int) in a malformed manifest -- iterate `_as_list()` rather
            # than the raw value so that reaches a clean skip instead of
            # `TypeError: 'int' object is not iterable`.
            for cap in _as_list(requires.get("capabilities")):
                # `cap` is schema-typed as a string, but a malformed
                # manifest can carry a dict/list there -- `cap not in
                # vocab` raises `TypeError: unhashable type` unfiltered.
                if not isinstance(cap, str) or cap not in vocab:
                    offered = ", ".join(sorted(vocab)) or "<none>"
                    msgs.append(
                        f"requires/capabilities[{cap}]: not a known SoC capability "
                        f"-- must be one the capability layer resolves "
                        f"(known: {offered})")

        if msgs:
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
            failures.append((rel, msgs))
        else:
            tier = doc.get("tier", "?")
            lic = doc.get("license", "?")
            print(f"OK   {rel}  (library: tier {tier}, {lic})")
    return failures


def _check_tier_a_library_ci(library_files, som_files) -> list:
    """Validate the Tier-A library CI registry against live metadata.

    The registry is the machine-readable contract the build workflow consumes
    and the portability matrix can cross-check later: every Tier-A library must
    either be in the host-build lane or carry an explicit exclusion reason, and
    every representative `(family, SoM, core)` cell must resolve against the SoM
    preset topology.
    """
    failures: list[tuple[Path, list[str]]] = []
    if not TIER_A_LIBRARY_CI_REGISTRY.is_file():
        return failures
    rel = TIER_A_LIBRARY_CI_REGISTRY.relative_to(REPO).as_posix()
    try:
        data = json.loads(TIER_A_LIBRARY_CI_REGISTRY.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"FAIL {rel}: parse error ({e})")
        return [(rel, [f"invalid JSON parse: {e}"])]

    # Unlike the other registry checks, this function keeps gathering
    # referential-integrity messages even when the schema pass below
    # already flagged a shape problem -- so a non-object top level must be
    # refused up front, before `data.get(...)` runs unconditionally further
    # down (same shape as `_check_silicon_kconfig`).
    if not isinstance(data, dict):
        msg = f"top-level value is a {type(data).__name__}, expected an object"
        print(f"FAIL {rel}: {msg}")
        return [(rel, [msg])]

    msgs: list[str] = []
    if TIER_A_LIBRARY_CI_SCHEMA.is_file():
        schema = json.loads(TIER_A_LIBRARY_CI_SCHEMA.read_text(encoding="utf-8"))
        validator = jsonschema.Draft202012Validator(schema)
        for err in sorted(validator.iter_errors(data), key=lambda e: list(e.absolute_path)):
            loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
            msgs.append(f"{loc}: {err.message}")

    library_docs: dict[str, dict] = {}
    for path in library_files:
        try:
            doc = strict_yaml_load(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue
        if isinstance(doc, dict) and isinstance(doc.get("name"), str):
            library_docs[doc["name"]] = doc

    tier_a = {name for name, doc in library_docs.items() if doc.get("tier") == "A"}
    host = _as_dict(data.get("hostBuild"))
    # `libraries`/`excludedLibraries` may themselves be a non-list/non-dict
    # scalar in a malformed registry -- `set(host.get("libraries") or [])`
    # used to reach `set(<int>)` (`TypeError: 'int' object is not
    # iterable`) and `.keys()` used to reach a non-dict directly
    # (`AttributeError`). Route both through the same container guards as
    # every other array/object field in this file. And a `libraries[]`
    # ITEM is schema-typed as a string, but a malformed registry can carry
    # a dict/list entry there -- `set()` raises `TypeError: unhashable
    # type` unfiltered, and a mixed str/int set raises on the `sorted()`
    # calls below. Filter to strings, same idiom used throughout this
    # file.
    host_libraries = {x for x in _as_list(host.get("libraries")) if isinstance(x, str)}
    excluded = set(_as_dict(host.get("excludedLibraries")).keys())
    known = set(library_docs)

    for name in sorted(host_libraries | excluded):
        if name not in known:
            msgs.append(f"hostBuild/{name}: no library manifest at metadata/libraries/{name}.yaml")
    for name in sorted(host_libraries):
        if library_docs.get(name, {}).get("tier") != "A":
            msgs.append(f"hostBuild/libraries[{name}]: library is not Tier A")
    for name in sorted(excluded):
        if library_docs.get(name, {}).get("tier") != "A":
            msgs.append(f"hostBuild/excludedLibraries[{name}]: library is not Tier A")

    accounted = host_libraries | excluded
    missing = tier_a - accounted
    extra = accounted - tier_a
    if missing:
        msgs.append("hostBuild: Tier-A libraries missing from build/exclusion set: "
                    + ", ".join(sorted(missing)))
    if extra:
        msgs.append("hostBuild: non-Tier-A libraries listed in build/exclusion set: "
                    + ", ".join(sorted(extra)))

    som_docs: dict[str, dict] = {}
    for path in som_files:
        try:
            doc = strict_yaml_load(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue
        if isinstance(doc, dict) and isinstance(doc.get("sku"), str):
            som_docs[doc["sku"]] = doc

    families_seen: set[str] = set()
    family_to_som: dict[str, str] = {}
    for idx, cell in enumerate(_as_list(data.get("familyMatrix"))):
        if not isinstance(cell, dict):
            continue
        family = cell.get("family")
        som = cell.get("som")
        core = cell.get("core")
        if isinstance(family, str):
            families_seen.add(family)
            if isinstance(som, str):
                family_to_som[family] = som
        if isinstance(som, (dict, list)):
            # A dict/list `som` is unhashable -- `som_docs.get(som)` below
            # would raise `TypeError: unhashable type`. Every other
            # schema-shape violation (int, bool, or a JSON `null`) IS
            # hashable and safe to look up; a blanket `not isinstance(som,
            # str)` would also silently drop the "has no SoM preset"
            # diagnostic `dcda807d` used to emit for a `null` `som` --
            # scope the skip to the actual hazard (unhashability), same as
            # the truthy-only skip in `_check_silicon_kconfig`'s
            # `knownSilicon[]` guard.
            continue
        doc = som_docs.get(som)
        if doc is None:
            msgs.append(f"familyMatrix[{idx}]/som: `{som}` has no SoM preset")
            continue
        if doc.get("family") != family:
            msgs.append(f"familyMatrix[{idx}]: family `{family}` does not match "
                        f"{som}'s preset family `{doc.get('family')}`")
        topology = doc.get("topology")
        # `topology` is schema-typed as an object, but a non-empty scalar
        # (e.g. a bare string) is truthy and would otherwise reach
        # `topology.get(core)` below and raise `AttributeError` -- normalise
        # to `{}` rather than crash the gate (same shape as
        # `_check_board_targets`).
        topology = topology if isinstance(topology, dict) else {}
        if isinstance(core, (dict, list)):
            # Same reasoning as `som` above -- `core not in topology` /
            # `topology.get(core)` below would raise on an unhashable
            # value, but every other value (int, bool, `null`) is
            # hashable and must still surface the `core` `is not a
            # topology core` diagnostic below.
            continue
        if core not in topology:
            # `topology` is a YAML mapping (unlike the JSON-sourced
            # `core_ids`/`macs` sets above) -- YAML permits int/float/bool/
            # null keys, so an unfiltered `sorted(topology)` over its keys
            # raises `TypeError` on a mixed str/non-str key set, or on an
            # all-non-str key set at the `join()` (non-str items). Filter
            # to strings, same idiom as `core_ids` above.
            available = ", ".join(sorted(k for k in topology if isinstance(k, str))) or "<none>"
            msgs.append(f"familyMatrix[{idx}]/core: `{core}` is not a topology core "
                        f"on {som} (available: {available})")
        elif not isinstance(topology.get(core), dict) or "board" not in topology[core]:
            msgs.append(f"familyMatrix[{idx}]/core: `{core}` on {som} is not a Zephyr slice")

    metadata_families = {
        doc.get("family")
        for doc in som_docs.values()
        if isinstance(doc.get("family"), str)
    }
    missing_families = metadata_families - families_seen
    if missing_families:
        msgs.append("familyMatrix: missing supported SoM families: "
                    + ", ".join(sorted(missing_families)))

    # `excludedFamilies` RATCHET (#1025 round-2 review): each entry claims
    # its family's SoM has no buildable hw_rev at all -- assert that against
    # live metadata the same way `excludedLibraries` above is asserted to
    # still be Tier A, instead of trusting the prose forever.
    for family, _reason in sorted(_as_dict(data.get("excludedFamilies")).items()):
        som = family_to_som.get(family)
        if som is None:
            msgs.append(f"excludedFamilies[{family}]: no familyMatrix cell "
                        f"for this family to check against")
            continue
        doc = som_docs.get(som)
        hw_rev = doc.get("default_hw_rev") if doc else None
        try:
            family_dir = _sku_family(som) if doc else None
        except ValueError:
            family_dir = None
        if not family_dir or not hw_rev:
            msgs.append(f"excludedFamilies[{family}]: cannot resolve "
                        f"family_dir/default_hw_rev for som `{som}`")
            continue
        stale = assert_exclusion_still_not_buildable(
            REPO / "metadata", family_dir, hw_rev,
            gate=f"tier-a-library-ci.json excludedFamilies[{family}]")
        if stale:
            msgs.append(stale)

    if msgs:
        print(f"FAIL {rel}")
        for m in msgs:
            print(f"  · {m}")
        failures.append((rel, msgs))
    else:
        n_libs = len(host_libraries)
        n_excluded = len(excluded)
        n_cells = len(_as_list(data.get("familyMatrix")))
        print(f"OK   {rel}  (hostBuild={n_libs}, excluded={n_excluded}, "
              f"familyMatrix={n_cells})")
    return failures


def _board_tree_identifiers() -> dict[str, set[str]]:
    """Map a bare Zephyr board name -> the set of fully-qualified twister
    identifiers its generated board tree exposes.

    Scans zephyr/boards/alp/<dir>/*.yaml for `identifier:` strings (the
    `<board>/<soc>/<cpucluster>` triple gen_zephyr_board emits).  The bare
    name is the identifier's first `/`-segment.  Returns {} when the board
    tree is absent (e.g. a metadata-only test root), which makes the
    board-target check a no-op rather than a false failure.
    """
    trees: dict[str, set[str]] = {}
    if not ZEPHYR_ALP_BOARDS.is_dir():
        return trees
    for path in sorted(ZEPHYR_ALP_BOARDS.glob("*/*.yaml")):
        try:
            doc = strict_yaml_load(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue
        if not isinstance(doc, dict):
            continue
        ident = doc.get("identifier")
        if isinstance(ident, str) and "/" in ident:
            trees.setdefault(ident.split("/", 1)[0], set()).add(ident)
    return trees


def _check_board_targets(som_files) -> list:
    """Cross-check every SoM preset `topology.<core>.board` against the
    generated Zephyr board trees (issue #720).

    The board string is passed verbatim to `west build -b`, so it must name
    a board Zephyr can resolve.  A multi-cluster SoC (Ensemble RTSS-HE/HP,
    RZ/V2N A55+M33) makes the *bare* board name ambiguous -- Zephyr 4.4
    needs the `<board>/<soc>/<cpucluster>` triple.  The generated board
    tree's twister `identifier:` is the ground truth for that triple.

    Invariant enforced (both drift directions):
      * a board whose tree EXISTS must be spelled as that tree's identifier
        (a bare name where the tree is qualified is the #720 bug; a wrong
        qualifier is drift); and
      * a `/`-qualified board must point at a tree that actually exists
        (guards against qualifying a SKU -- e.g. V2N102/V2M102 -- before its
        board tree is generated).

    A bare board with no generated tree is left alone HERE -- this check's
    only job is qualification-form drift against a tree that exists, not
    existence itself.  Whether that bare board should have a tree at all
    (single-cluster target Zephyr resolves as-is, vs. a not-yet-generated
    board that cannot build regardless) is `check_board_target_tree_parity.py`'s
    job: it requires the gap be declared in its `_NOT_YET_SUPPORTED`
    allowlist, with a reason, rather than silently unbuildable.  Returns a
    failure list shaped like _check_files().
    """
    failures: list[tuple[Path, list[str]]] = []
    trees = _board_tree_identifiers()
    if not trees:
        return failures  # no board tree in this root -> nothing to cross-check
    for path in som_files:
        try:
            rel = path.relative_to(REPO).as_posix()
        except ValueError:
            rel = path.as_posix()
        try:
            doc = strict_yaml_load(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # schema pass already reported the parse failure
        if not isinstance(doc, dict):
            continue

        msgs: list[str] = []
        checked = 0
        raw_topology = doc.get("topology")
        if raw_topology is not None and not isinstance(raw_topology, dict):
            # `topology` is schema-typed as an object, but a non-empty
            # scalar (e.g. a bare string, which is truthy) would otherwise
            # reach `.items()` below and raise `AttributeError`, aborting
            # the whole gate mid-run instead of leaving the schema FAIL
            # line (which already explains the real problem) to do the
            # talking (same shape as `_check_chip_physical`'s `physical`
            # guard). `_as_dict` alone is NOT equivalent here: it would
            # degrade this to `{}` and fall through to `checked == 0` ->
            # an `OK ... (board targets: 0 Zephyr slice(s) resolve)` line
            # printed for a file the schema pass FAILs in the same run --
            # skip the file instead so this check stays silent on it.
            continue
        topology = _as_dict(raw_topology)
        for core_id, entry in topology.items():
            if not isinstance(entry, dict):
                continue
            board = entry.get("board")
            if not isinstance(board, str) or not board:
                continue  # yocto slice / no Zephyr board on this core
            checked += 1
            bare = board.split("/", 1)[0]
            if bare in trees:
                # A tree exists -> the board must be its qualified identifier.
                if board not in trees[bare]:
                    want = sorted(trees[bare])
                    want_str = want[0] if len(want) == 1 else ", ".join(want)
                    msgs.append(
                        f"topology/{core_id}/board: `{board}` does not match the "
                        f"generated board tree for `{bare}` -- `west build -b` "
                        f"needs the fully-qualified `{want_str}` "
                        f"(zephyr/boards/alp/; #720)")
            elif "/" in board:
                # Qualified, but no tree with that bare name exists.
                msgs.append(
                    f"topology/{core_id}/board: `{board}` is qualified but no "
                    f"generated board tree named `{bare}` exists under "
                    f"zephyr/boards/alp/ -- qualify a board only once its tree "
                    f"is generated (#720)")

        if msgs:
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
            failures.append((rel, msgs))
        else:
            print(f"OK   {rel}  (board targets: {checked} Zephyr slice(s) resolve)")
    return failures


def main() -> int:
    # SoC files (JSON) against soc-spec v1.
    soc_schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    soc_validator = jsonschema.Draft202012Validator(soc_schema)
    soc_files = sorted(SOCS.rglob("*.json"))
    if not soc_files:
        print(f"no SoC metadata files found under {SOCS}", file=sys.stderr)
        return 1
    soc_failures = _check_files(
        "JSON", soc_files, soc_validator,
        lambda p: strict_json_loads(p.read_text(encoding="utf-8"), source=p),
        "ref",
    )
    # Semantic cross-ref the schema can't express: npus[].paired_core -> cores[].
    soc_failures += _check_soc_npu_pairing(soc_files)
    # Semantic cross-ref the schema can't express: variants[].debug.jlink_device keys -> cores[].
    soc_failures += _check_soc_debug_probe_identity(soc_files)
    # #1295: every Alif Ensemble variant must declare debug.jlink_flash_device (string or null) -- never omit it.
    soc_failures += _check_soc_jlink_flash_device_declared(soc_files)
    # #1444: Alp Lab modules are BGA only -- no Alif Ensemble variant may declare a WLCSP package.
    soc_failures += _check_soc_no_wlcsp_variants(soc_files)

    # SoM preset files (YAML) against som-preset v1.
    som_validator = None
    som_failures: list = []
    som_files: list = []
    if SOM_SCHEMA.is_file():
        som_schema = json.loads(SOM_SCHEMA.read_text(encoding="utf-8"))
        som_validator = jsonschema.Draft202012Validator(som_schema)
        som_files = sorted(SOM_PRESETS.glob("E1M-*.yaml"))
        if som_files:
            print()
            som_failures = _check_files(
                "YAML", som_files, som_validator,
                lambda p: strict_yaml_load(p.read_text(encoding="utf-8"), source=p),
                "sku",
            )

    # Per-family hw-revisions files (YAML) against hw-revisions v1.
    hwrev_failures: list = []
    hwrev_files: list = []
    if HWREV_SCHEMA.is_file():
        hwrev_schema = json.loads(HWREV_SCHEMA.read_text(encoding="utf-8"))
        hwrev_validator = jsonschema.Draft202012Validator(hwrev_schema)
        hwrev_files = sorted(SOM_PRESETS.glob("*/hw-revisions.yaml"))
        if hwrev_files:
            print()
            hwrev_failures = _check_files(
                "YAML", hwrev_files, hwrev_validator,
                lambda p: strict_yaml_load(p.read_text(encoding="utf-8"), source=p),
                "family",
            )

    # Shared board presets (YAML) against the board-preset schema.
    # Distinct from project board.yaml files (board.schema.json /
    # scripts/validate_board_yaml.py): these are the SDK-internal
    # shared board definitions referenced via `preset:`.
    board_failures: list = []
    board_files: list = []
    if BOARD_PRESET_SCHEMA.is_file():
        board_schema = json.loads(BOARD_PRESET_SCHEMA.read_text(encoding="utf-8"))
        board_validator = jsonschema.Draft202012Validator(board_schema)
        board_files = sorted(BOARD_PRESETS.glob("*.yaml"))
        if board_files:
            print()
            board_failures = _check_files(
                "YAML", board_files, board_validator,
                lambda p: strict_yaml_load(p.read_text(encoding="utf-8"), source=p),
                "name",
            )

    # Chip manifests (YAML) against chip-v1 schema.
    chip_failures: list = []
    chip_files: list = []
    if CHIP_SCHEMA.is_file():
        chip_schema = json.loads(CHIP_SCHEMA.read_text(encoding="utf-8"))
        chip_validator = jsonschema.Draft202012Validator(chip_schema)
        chip_files = sorted(CHIPS.glob("*.yaml"))
        if chip_files:
            print()
            chip_failures = _check_files(
                "YAML", chip_files, chip_validator,
                lambda p: strict_yaml_load(p.read_text(encoding="utf-8"), source=p),
                "chip_id",
            )
            chip_failures += _check_chip_semantics(chip_files)
            chip_failures += _check_chip_physical(chip_files)

    # Block manifests (YAML) against block-v1 schema.
    block_failures: list = []
    block_files: list = []
    if BLOCK_SCHEMA.is_file():
        block_schema = json.loads(BLOCK_SCHEMA.read_text(encoding="utf-8"))
        block_validator = jsonschema.Draft202012Validator(block_schema)
        block_files = sorted(BLOCKS.glob("*.yaml"))
        if block_files:
            print()
            block_failures = _check_files(
                "YAML", block_files, block_validator,
                lambda p: strict_yaml_load(p.read_text(encoding="utf-8"), source=p),
                "block_id",
            )
            block_failures += _check_block_realizations(block_files, chip_files)

    # Library manifests (YAML) against library v1 (ADR 0018).
    library_failures: list = []
    library_semantic_failures: list = []
    library_files: list = []
    if LIBRARY_SCHEMA.is_file():
        library_schema = json.loads(LIBRARY_SCHEMA.read_text(encoding="utf-8"))
        library_validator = jsonschema.Draft202012Validator(library_schema)
        library_files = sorted(LIBRARIES.glob("*.yaml"))
        if library_files:
            print()
            library_failures = _check_files(
                "YAML", library_files, library_validator,
                lambda p: strict_yaml_load(p.read_text(encoding="utf-8"), source=p),
                "name",
            )
            library_semantic_failures = _check_library_semantics(library_files)

    # SoM `topology.<core>.board` <-> generated Zephyr board tree cross-check.
    board_target_failures: list = []
    if som_files:
        print()
        board_target_failures = _check_board_targets(som_files)

    # SoM `silicon_capabilities.unpopulated` <-> SoC capability cross-check.
    restriction_failures: list = []
    if som_files:
        print()
        restriction_failures = _check_silicon_capability_restrictions(som_files)

    # SoM `soc_peripheral_instances[].instance` slug uniqueness.
    instance_uniqueness_failures: list = []
    if som_files:
        print()
        instance_uniqueness_failures = _check_som_peripheral_instance_uniqueness(som_files)

    # SoM `memory_map:` `*_slot0` regions must resolve a concrete address.
    slot0_address_failures: list = []
    if som_files:
        print()
        slot0_address_failures = _check_som_slot0_address_resolved(som_files)

    # Silicon -> Kconfig registry + socs/ correspondence.
    print()
    silicon_kconfig_failures = _check_silicon_kconfig()
    peripheral_kconfig_failures = _check_peripheral_kconfig()

    # ADR 0018 Tier-A library CI registry + metadata correspondence.
    print()
    tier_a_library_ci_failures = _check_tier_a_library_ci(library_files, som_files)

    print()
    total_failures = (len(soc_failures) + len(som_failures)
                      + len(hwrev_failures) + len(board_failures) + len(chip_failures)
                      + len(block_failures)
                      + len(library_failures) + len(library_semantic_failures)
                      + len(board_target_failures)
                      + len(restriction_failures)
                      + len(instance_uniqueness_failures)
                      + len(slot0_address_failures)
                      + len(silicon_kconfig_failures)
                      + len(peripheral_kconfig_failures)
                      + len(tier_a_library_ci_failures))
    print(f"{len(soc_files)} SoC file(s) + {len(som_files)} SoM preset(s) + "
          f"{len(hwrev_files)} hw-revisions file(s) + "
          f"{len(board_files)} board preset(s) + {len(chip_files)} chip file(s) + "
          f"{len(block_files)} block file(s) + "
          f"{len(library_files)} library manifest(s) + Kconfig registries + "
          f"tier-a-library-ci registry "
          f"checked, {total_failures} failure(s)")
    return 0 if total_failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
