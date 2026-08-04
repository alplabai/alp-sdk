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
        silicon = str(doc.get("silicon", ""))
        soc_path = resolve_soc_path(silicon, SOCS.parent)
        if soc_path is None or not soc_path.is_file():
            msgs.append(f"silicon_capabilities: silicon ref `{silicon}` does not "
                        f"resolve to a metadata/socs/ spec, cannot validate "
                        f"`unpopulated:` against the silicon capability set")
        else:
            soc_doc = json.loads(soc_path.read_text(encoding="utf-8"))
            soc_caps = soc_doc.get("capabilities") or {}

        som_caps = doc.get("capabilities") or {}
        for name in unpopulated:
            if soc_path is not None and soc_path.is_file() and not soc_caps.get(name):
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

    msgs: list[str] = []
    if SILICON_KCONFIG_SCHEMA.is_file():
        schema = json.loads(SILICON_KCONFIG_SCHEMA.read_text(encoding="utf-8"))
        validator = jsonschema.Draft202012Validator(schema)
        for err in sorted(validator.iter_errors(data), key=lambda e: list(e.absolute_path)):
            loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
            msgs.append(f"{loc}: {err.message}")

    for ref in data.get("knownSilicon", []):
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
        n = len(data.get("knownSilicon", []))
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
        n = len(data.get("peripherals", {}))
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
        npus = doc.get("npus") or []
        if not npus:
            continue
        rel = path.relative_to(REPO).as_posix()
        core_ids = {c.get("id") for c in (doc.get("cores") or []) if c.get("id")}
        msgs: list[str] = []

        # (1) referential integrity of every declared paired_core.
        for i, n in enumerate(npus):
            pc = n.get("paired_core")
            if pc is not None and pc not in core_ids:
                msgs.append(
                    f"npus[{i}] ({n.get('type')}/{n.get('subtype')}): "
                    f"paired_core={pc!r} is not a cores[].id "
                    f"(known: {sorted(core_ids)})")

        # (2) multi-MAC variants must pair every instance to a core.
        by_type: dict[str, list[dict]] = {}
        for n in npus:
            by_type.setdefault(str(n.get("type", "")), []).append(n)
        for ntype, insts in by_type.items():
            macs = {n.get("mac_per_cycle") for n in insts if n.get("mac_per_cycle")}
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
    """Cross-ref `variants[].debug.jlink_device` keys against `cores[].id`.

    #987 publishes the debug-probe identity (J-Link device, pyOCD target)
    per variant, with `jlink_device` keyed by core id since a J-Link attach
    device can in principle differ per core class (a future non-M55 core
    would need a different value, even though every Alif M55 core today
    shares the generic `Cortex-M55` string).  JSON Schema can express that
    `jlink_device` is an object of string values but not that its *keys*
    are real cores on *this* SoC -- a typo (or a stale key surviving a core
    rename) would silently point the extension's launch-config generator at
    a core that does not exist.  Enforce it here.

    Returns a failure list shaped like `_check_files()`.
    """
    failures: list[tuple[Path, list[str]]] = []
    for path in soc_files:
        try:
            doc = strict_json_loads(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # parse errors already reported by the schema pass
        variants = doc.get("variants") or []
        if not variants:
            continue
        rel = path.relative_to(REPO).as_posix()
        core_ids = {c.get("id") for c in (doc.get("cores") or []) if c.get("id")}
        msgs: list[str] = []

        for i, v in enumerate(variants):
            jlink_device = ((v.get("debug") or {}).get("jlink_device")) or {}
            for core_id in jlink_device:
                if core_id not in core_ids:
                    msgs.append(
                        f"variants[{i}] ({v.get('order_code')}): "
                        f"debug.jlink_device key {core_id!r} is not a "
                        f"cores[].id (known: {sorted(core_ids)})")

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
        sig_names = {s["name"] for s in doc.get("signals", []) if isinstance(s, dict) and "name" in s}
        msgs: list = []
        seen_pads: dict = {}
        for pin in phys.get("pins", []):
            sig = pin.get("signal"); pad = pin.get("pad")
            if sig not in sig_names and sig not in _POWER_NETS:
                msgs.append(f"physical.pins pad {pad}: signal '{sig}' not in signals[] or power nets")
            if pad in seen_pads:
                msgs.append(f"physical.pins: pad '{pad}' used more than once")
            seen_pads[pad] = True
        for passive in phys.get("passives", []):
            net = passive.get("net")
            if net not in sig_names and net not in _POWER_NETS:
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
        iface = {e["signal"] for e in doc.get("interface", []) if isinstance(e, dict) and "signal" in e}
        msgs: list = []
        for r in doc.get("realizations", []):
            for part in r.get("parts", []):
                if part.get("chip") not in chip_ids:
                    msgs.append(f"realization '{r.get('id')}': part chip '{part.get('chip')}' has no metadata/chips manifest")
                for _pin, sig in (part.get("maps") or {}).items():
                    if sig not in iface:
                        msgs.append(f"realization '{r.get('id')}': maps target '{sig}' not in interface[]")
            for passive in r.get("passives", []):
                net = passive.get("net")
                if net not in iface and net not in _POWER_NETS:
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
            for cap in requires.get("capabilities") or []:
                if cap not in vocab:
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
    host = data.get("hostBuild", {}) if isinstance(data.get("hostBuild"), dict) else {}
    host_libraries = set(host.get("libraries") or [])
    excluded = set((host.get("excludedLibraries") or {}).keys())
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
    for idx, cell in enumerate(data.get("familyMatrix") or []):
        if not isinstance(cell, dict):
            continue
        family = cell.get("family")
        som = cell.get("som")
        core = cell.get("core")
        if isinstance(family, str):
            families_seen.add(family)
            if isinstance(som, str):
                family_to_som[family] = som
        doc = som_docs.get(som)
        if doc is None:
            msgs.append(f"familyMatrix[{idx}]/som: `{som}` has no SoM preset")
            continue
        if doc.get("family") != family:
            msgs.append(f"familyMatrix[{idx}]: family `{family}` does not match "
                        f"{som}'s preset family `{doc.get('family')}`")
        topology = doc.get("topology") or {}
        if core not in topology:
            available = ", ".join(sorted(topology)) or "<none>"
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
    for family, _reason in sorted((data.get("excludedFamilies") or {}).items()):
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
        n_cells = len(data.get("familyMatrix") or [])
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
        topology = doc.get("topology") or {}
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
