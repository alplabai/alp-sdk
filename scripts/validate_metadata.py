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

import datetime
import hashlib
import json
import re
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
NPU_OPS_SCHEMA = REPO / "metadata" / "schemas" / "npu-ops-v1.schema.json"
NPU_OPS = REPO / "metadata" / "npu_ops"
MODEL_PERF_SCHEMA = REPO / "metadata" / "schemas" / "model-perf-v1.schema.json"
MODEL_PERF = REPO / "metadata" / "model_perf"
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


#: `silicon:` refs on which `on_module.hyperram` + `on_module.ospi_memories`
#: are MANDATORY, not merely bindable-if-present.  Matched on the ref rather
#: than a SKU allow-list so a future E1M-AEN901 is covered the day it lands.
#: The Alif Ensemble die's only external memory interface is the OSPI/HexSPI
#: octal bus (`metadata/socs/alif/ensemble/e8.json` `external_memory_interfaces`
#: lists HexSPI + SD/eMMC and no DRAM), so on this family those two blocks are
#: not one possible source for `memory:` -- they are its whole source.
_ALIF_ENSEMBLE_SILICON_PREFIX = "alif:ensemble:"


def _population_state(entry: dict) -> str:
    """Project an `assembled:` key onto `fitted` / `optional` / `absent`.

    The schema's own default is authoritative: *"Population status: true
    (default), false (DNI), or \"optional\" (assembled per BOM variant)"* --
    an entry with no `assembled` key describes a part that IS fitted, so a
    missing key must read `fitted`, never "unknown".
    """
    raw = entry.get("assembled", True)
    if raw is False:
        return "absent"
    if raw == "optional":
        return "optional"
    return "fitted"


def _memory_population_msgs(
    figure_key: str,
    figure,
    parts: "list[tuple[str, dict]]",
) -> "list[str]":
    """Bind ONE `memory.<figure_key>` against the population of its parts.

    `parts` is `[(dotted_path, entry)]` -- every `on_module` part whose
    population decides this figure.  Returns the failure messages, empty
    when the figure and the population agree.
    """
    if not parts:
        return []

    # `True`/`False` are `int` subclasses in Python; the schema forbids a
    # boolean here, but never let one read as the integer 0/1.
    is_int = isinstance(figure, int) and not isinstance(figure, bool)
    fitted = [(n, e) for n, e in parts if _population_state(e) == "fitted"]
    optional = [(n, e) for n, e in parts if _population_state(e) == "optional"]
    populating = fitted + optional
    names = ", ".join(n for n, _ in parts)

    if not populating:
        # Every declared part is `assembled: false` -- the population
        # question is ANSWERED, and the answer is "none".  That is `0`.
        # `TBD` would re-open a question the preset just closed, and any
        # positive figure claims memory the module demonstrably has not
        # got (which is exactly the 32 MiB #915 deleted).
        if not (is_int and figure == 0):
            return [
                f"memory.{figure_key}={figure!r} but every part that could "
                f"carry it is `assembled: false` ({names}) -- a resolved "
                f"'populates none' is `0`, never TBD and never a capacity"
            ]
        return []

    populating_names = ", ".join(n for n, _ in populating)
    if is_int and figure == 0:
        # `0` means "no such part on any current BOM variant"; at least one
        # part says otherwise.  This is the mutation that used to be FULLY
        # GREEN: `hyperram.assembled: true` next to `dram_mbit: 0`.
        # Name each offender WITH its own state -- a mixed fitted/optional
        # set must not be reported under one blanket `assembled:` value.
        stated = ", ".join(
            f"{n} (`assembled: {'optional' if _population_state(e) == 'optional' else 'true'}`)"
            for n, e in populating)
        return [
            f"memory.{figure_key}=0 claims the module populates no such "
            f"part, but {stated} is populated"
        ]

    if optional:
        # BOM-variant dependent: the capacity of the variant that DOES
        # fit the part is a maintainer call, so only the `0` contradiction
        # above is decidable here.
        return []

    caps = [e.get("capacity_mbit") for _, e in fitted]
    if not all(isinstance(c, int) and not isinstance(c, bool) for c in caps):
        # A fitted part whose own capacity is TBD leaves the module figure
        # genuinely underivable -- nothing to cross-check against.
        return []

    expected = sum(int(c) for c in caps)
    if figure != expected:
        return [
            f"memory.{figure_key}={figure!r} does not match the parts it is "
            f"derived from: {populating_names} "
            f"{'sum to' if len(fitted) > 1 else 'declares'} "
            f"capacity_mbit={expected}"
        ]
    return []


def _check_som_memory_population(som_files) -> list:
    """Bind `memory:` to the `on_module` population facts it is DERIVED from.

    Every AEN preset carries a comment stating the derivation
    (`metadata/e1m_modules/E1M-AEN801.yaml`: *"dram_mbit  <- 0:
    on_module.hyperram is `assembled: false`"*), and until this check
    landed a comment was the whole of the enforcement.  Proven by
    mutation: setting `hyperram.assembled: true` while leaving
    `dram_mbit: 0` was FULLY GREEN -- `validate_metadata.py` rc=0 AND
    `pytest tests/scripts/` rc=0 -- and `dram_mbit: 128` against an
    unpopulated part left only one hardcoded string assertion red.  A
    derivation nothing binds is not a derivation; it is a comment that
    happens to be true today.

    The rules, per figure:

      - `memory.dram_mbit` is decided by `on_module.hyperram`;
        `memory.flash_mbit` by every `on_module.ospi_memories[]` entry.
        A preset that declares neither block (V2N/V2M's LPDDR4X + eMMC,
        E1M-NX9101's open capacities) states no population fact here and
        is skipped -- there is nothing to bind to, and inventing one
        would be inventing a hardware value.  A skipped preset prints an
        explicit `SKIP <rel> (nothing bound ...)` line, so "this file was
        not cross-checked" is a thing you can READ in the gate's output
        rather than an absence you have to notice.  The first version of
        this check printed nothing at all for an unbound preset.
      - EXCEPT on an Alif Ensemble part (`silicon: alif:ensemble:*`),
        where both blocks are REQUIRED.  Skipping-when-absent is the
        right default for a family whose external memory the SDK has no
        model of, but on Ensemble the OSPI/HexSPI octal bus is the ONLY
        external memory interface the die has -- `on_module.hyperram`
        and `on_module.ospi_memories` are not one possible source for
        `memory:`, they are its whole source.  Omitting them there does
        not leave the question open, it DELETES the fact this check
        binds to: measured, `_check_som_memory_population([synthetic])`
        returned `[]` for an AEN preset carrying `dram_mbit: 256` with
        no `on_module` memory blocks at all, i.e. a NEW AEN SKU could
        restate the exact 32-MiB-of-HyperRAM claim #915 had just deleted
        and ship it at rc=0.  Derived from the `silicon:` ref rather
        than a SKU allow-list, so an E1M-AEN901 added tomorrow is
        covered the day it lands.
      - Every relevant part `assembled: false` => the figure MUST be `0`.
        This is the `0`-vs-`TBD` distinction #915 established: `0` is a
        RESOLVED fact ("populates none"), `TBD` is an open question
        ("nobody has written the capacity down yet", E1M-NX9101's state).
        A preset that has answered the question may not then spell the
        answer `TBD`, and may not claim a capacity either.
      - Any relevant part populated (`assembled: true`, or the key
        absent -- the schema's own default) => the figure MUST NOT be
        `0`, and when every fitted part declares an integer
        `capacity_mbit` it must equal their sum.
      - `assembled: "optional"` is BOM-variant dependent, so only the `0`
        contradiction is decidable; the exact capacity is not.

    JSON Schema cannot reach across `on_module` into `memory:` (nor sum
    a sibling object's values), so this is the only layer that can hold
    the derivation.  Returns a failure list shaped like `_check_files()`.
    """
    failures: list[tuple[str, list[str]]] = []
    for path in som_files:
        rel = path.relative_to(REPO).as_posix()
        try:
            doc = strict_yaml_load(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # parse errors already reported by the schema pass
        if not isinstance(doc, dict):
            continue
        memory = doc.get("memory")
        if not isinstance(memory, dict):
            memory = {}
        on_module = doc.get("on_module")
        if not isinstance(on_module, dict):
            on_module = {}

        msgs: list[str] = []
        bound: list[str] = []

        hyperram = on_module.get("hyperram")
        ospi = on_module.get("ospi_memories")

        # An Ensemble part's ONLY external memory sits on the OSPI/HexSPI
        # octal bus, so declaring the blocks is not optional there: without
        # them `memory:` is unbindable and can claim anything at rc=0.
        silicon = doc.get("silicon")
        if isinstance(silicon, str) and \
                silicon.startswith(_ALIF_ENSEMBLE_SILICON_PREFIX):
            for key, block, figure in (
                    ("hyperram", hyperram, "dram_mbit"),
                    ("ospi_memories", ospi, "flash_mbit")):
                if not isinstance(block, dict) or not block:
                    msgs.append(
                        f"silicon={silicon!r} is an Alif Ensemble part, whose "
                        f"only external memory sits on the OSPI/HexSPI octal "
                        f"bus, so `memory.{figure}` is DERIVED from "
                        f"`on_module.{key}` -- but that block is missing or "
                        f"empty. Omitting it does not leave the question open, "
                        f"it deletes the fact this check binds `memory.{figure}"
                        f"` to. Declare the part with `assembled:` (see "
                        f"metadata/e1m_modules/E1M-AEN801.yaml), `assembled: "
                        f"false` if the SKU populates none")

        if isinstance(hyperram, dict):
            bound.append("dram_mbit <- on_module.hyperram")
            msgs += _memory_population_msgs(
                "dram_mbit", memory.get("dram_mbit"),
                [("on_module.hyperram", hyperram)])

        if isinstance(ospi, dict) and ospi:
            entries = [(f"on_module.ospi_memories.{k}", v)
                       for k, v in sorted(ospi.items()) if isinstance(v, dict)]
            if entries:
                bound.append("flash_mbit <- on_module.ospi_memories")
                msgs += _memory_population_msgs(
                    "flash_mbit", memory.get("flash_mbit"), entries)

        if msgs:
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
            failures.append((rel, msgs))
        elif bound:
            print(f"OK   {rel}  ({'; '.join(bound)})")
        else:
            # Printed, not omitted: an unbound preset that produced NO line
            # was indistinguishable from one this loop never reached.
            print(f"SKIP {rel}  (nothing bound -- declares neither "
                  f"on_module.hyperram nor on_module.ospi_memories)")
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
        # `npus[]`/`cores[]` entries are schema-typed objects, but the
        # schema pass that would reject a malformed one is not guaranteed to
        # have run first -- filter to dicts rather than let a non-object
        # raise `AttributeError` here and abort the whole gate mid-run,
        # hiding the schema FAIL line that already explains the real
        # problem (same shape as `_check_soc_vela_memory_profile`).
        npus = [n for n in (doc.get("npus") or []) if isinstance(n, dict)]
        if not npus:
            continue
        rel = path.relative_to(REPO).as_posix()
        core_ids = {c.get("id") for c in (doc.get("cores") or [])
                    if isinstance(c, dict) and c.get("id")}
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


# `[Memory_Mode.*]` sections shipped in Arm's own vela.ini (ethos-u-vela
# 5.1.0) whose `arena_mem_area` is `Axi1` -- the non-SRAM memory that same
# file documents as "assumed to be read-writeable", i.e. DRAM.  Vela's own
# no-flags default (`Dedicated_Sram_384KB`) is in this set, which is exactly
# why a DRAM-less part must never inherit it.
_VELA_DRAM_BACKED_MEMORY_MODES = {
    "Dedicated_Sram",
    "Dedicated_Sram_256KB",
    "Dedicated_Sram_384KB",
    "Dedicated_Sram_512KB",
}

# `[System_Config.*]` sections shipped in that same Arm vela.ini.  Anything
# else exists only in a vendor config; passing it without `--config` is a hard
# vela rc=1, not a degradation.
_VELA_BUILTIN_SYSTEM_CONFIGS = {
    "Ethos_U55_Deep_Embedded",
    "Ethos_U55_High_End_Embedded",
    "Ethos_U65_Embedded",
    "Ethos_U65_Mid_End",
    "Ethos_U65_High_End",
    "Ethos_U65_Client_Server",
    "Ethos_U85_SYS_Flash_Low",
    "Ethos_U85_SYS_Flash_High",
    "Ethos_U85_SYS_DRAM_Low",
    "Ethos_U85_SYS_DRAM_Mid",
    "Ethos_U85_SYS_DRAM_High",
}


def _check_soc_vela_memory_profile(soc_files) -> list:
    """Cross-check `npu_toolchain.vela` against the rest of the SAME SoC spec.

    `--emit build-plan`'s consumer derives vela's `--memory-mode` from the SKU
    rather than letting vela fall back to `Dedicated_Sram_384KB`, which places
    the whole working set in DRAM and reports `sram_memory_used = 0.0`.  Zero
    then satisfies alp-sdk's on-device fit gate against ANY arena
    (src/backends/inference/alp_model_select.c), so a wrong profile is worse
    than none.  JSON Schema cannot reach the sibling fields these invariants
    need, so enforce them here:

      1. every SoC declaring an `ethos-u*` NPU carries the block, and no SoC
         without one does (vela compiles for nothing else);
      2. a `Dedicated_Sram*` memory_mode puts the arena in read-writeable
         non-SRAM, so the SoC must declare a DRAM-class
         `external_memory_interfaces` entry -- the Alif Ensemble parts declare
         only OctalSPI/HexSPI + SD/eMMC and must never claim one;
      3. a scalar `system_config` describes ONE accelerator (on Alif, one core
         subsystem), so it is legal only on a SoC carrying exactly one
         distinct Ethos-U `(type, subtype)`;
      4. a `system_config` outside Arm's built-in set must be flagged
         `system_config_requires_vendor_config: true` AND name its file, else
         a consumer would put an unresolvable section on the command line.

    Reading the `source` citations back -- proving the cited lines still state
    the declared `memory_mode` -- deliberately does NOT live here. Those
    citations point into `examples/` and `vendors/`, and this script is run
    against a metadata-ONLY scratch clone by
    tests/scripts/test_alp_cli_new_som.py's
    `_clone_metadata_gates`, where those trees do not exist. Making the check
    tolerate their absence would turn it into a silent skip; it lives in
    tests/scripts/test_vela_profile_metadata.py instead, which always runs
    against the real checkout.

    Returns a failure list shaped like `_check_files()`.
    """
    failures: list[tuple[Path, list[str]]] = []
    for path in soc_files:
        try:
            doc = strict_json_loads(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # parse errors already reported by the schema pass
        rel = path.relative_to(REPO).as_posix()
        # Every list read out of the parsed doc here (`npus`,
        # `external_memory_interfaces`) is schema-typed as a list of objects,
        # but the schema pass that would reject a malformed entry (e.g. a
        # bare string or a list) runs separately and is not guaranteed to
        # have run first -- filter to dicts rather than let a non-object
        # raise `AttributeError: '<type>' object has no attribute 'get'`
        # here and abort the whole gate mid-run, hiding the schema FAIL line
        # that already explains the real problem.  Same reasoning as
        # `npu_toolchain` below, and the same shape as the fix in
        # `_check_soc_npu_pairing`.
        npus = [n for n in (doc.get("npus") or []) if isinstance(n, dict)]
        ethos = [n for n in npus if str(n.get("type", "")).startswith("ethos-u")]
        npu_toolchain = doc.get("npu_toolchain")
        npu_toolchain = npu_toolchain if isinstance(npu_toolchain, dict) else {}
        vela = npu_toolchain.get("vela")
        vela = vela if isinstance(vela, dict) else {}
        msgs: list[str] = []

        # (1) presence is decided by the accelerator the SoC actually carries.
        if ethos and not vela:
            msgs.append(
                "declares an Ethos-U NPU but no npu_toolchain.vela -- a consumer "
                "would inherit vela's DRAM-backed default profile")
        if vela and not ethos:
            msgs.append(
                "declares npu_toolchain.vela but no ethos-u* NPU -- vela does not "
                "compile for this accelerator")

        if vela and ethos:
            mode = vela.get("memory_mode")

            # (2) a DRAM-backed placement needs a DRAM interface on this part.
            kinds = [str(e.get("kind", ""))
                     for e in (doc.get("external_memory_interfaces") or [])
                     if isinstance(e, dict)]
            has_dram = any("DDR" in k.upper() for k in kinds)
            if mode in _VELA_DRAM_BACKED_MEMORY_MODES and not has_dram:
                msgs.append(
                    f"npu_toolchain.vela.memory_mode={mode!r} places the tensor arena "
                    f"in read-writeable non-SRAM, but external_memory_interfaces "
                    f"{kinds} declares no DRAM")

            sysconf = vela.get("system_config")
            if sysconf is not None:
                # (3) one System_Config cannot describe several accelerators.
                identities = {(n.get("type"), n.get("subtype")) for n in ethos}
                if len(identities) != 1:
                    msgs.append(
                        f"npu_toolchain.vela.system_config={sysconf!r} is a single "
                        f"section name but this SoC carries {len(identities)} distinct "
                        f"Ethos-U accelerators "
                        f"{sorted(str(i) for i in identities)} -- a System_Config "
                        f"describes one accelerator (on Alif, one core subsystem)")
                # (4) a vendor section is unusable without its file.
                if sysconf not in _VELA_BUILTIN_SYSTEM_CONFIGS:
                    if vela.get("system_config_requires_vendor_config") is not True:
                        msgs.append(
                            f"npu_toolchain.vela.system_config={sysconf!r} is not an Arm "
                            f"built-in but system_config_requires_vendor_config is not "
                            f"true -- a consumer would pass an unresolvable section and "
                            f"vela would exit 1")
                    if not vela.get("vendor_config_filename"):
                        msgs.append(
                            f"npu_toolchain.vela.system_config={sysconf!r} needs a vendor "
                            f"config but vendor_config_filename is unset")

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
        # `variants[]`/`cores[]` entries are schema-typed objects, but the
        # schema pass that would reject a malformed one is not guaranteed to
        # have run first -- filter to dicts rather than let a non-object
        # raise `AttributeError`/`TypeError` here and abort the whole gate
        # mid-run, hiding the schema FAIL line that already explains the
        # real problem (same shape as `_check_soc_vela_memory_profile`).
        variants = [v for v in (doc.get("variants") or []) if isinstance(v, dict)]
        if not variants:
            continue
        rel = path.relative_to(REPO).as_posix()
        cores = [c for c in (doc.get("cores") or []) if isinstance(c, dict)]
        core_ids = {c.get("id") for c in cores if c.get("id")}
        # Cortex-M cores only for the `expect_dpidr` pairing rule below: the
        # DPIDR preflight guards the Zephyr-on-M J-Link flash path, and
        # `debug.jlink_device` is legitimately sparse across `cores[]` --
        # E8 publishes an attach profile for m55_hp/m55_he and none for
        # a32_cluster, an A-cluster that boots Linux off storage rather than
        # being J-Link flashed. Demanding coverage of every core would fail
        # the very variant this rule exists to protect.
        m_core_ids = {
            c["id"] for c in cores
            if c.get("id") and str(c.get("type") or "").startswith("cortex-m")
        }
        msgs: list[str] = []

        for i, v in enumerate(variants):
            debug = v.get("debug") or {}
            jlink_device = debug.get("jlink_device") or {}
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
        if doc.get("vendor") != "Alif Semiconductor" or doc.get("family") != "Ensemble":
            continue
        # `variants[]` entries are schema-typed objects, but the schema pass
        # that would reject a malformed one is not guaranteed to have run
        # first -- filter to dicts rather than let a non-object raise
        # `AttributeError` here (same shape as
        # `_check_soc_vela_memory_profile`).
        variants = [v for v in (doc.get("variants") or []) if isinstance(v, dict)]
        if not variants:
            continue
        rel = path.relative_to(REPO).as_posix()
        msgs: list[str] = []

        for i, v in enumerate(variants):
            debug = v.get("debug") or {}
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
        if doc.get("vendor") != "Alif Semiconductor" or doc.get("family") != "Ensemble":
            continue
        rel = path.relative_to(REPO).as_posix()
        msgs: list[str] = []

        # `variants[]` entries are schema-typed objects, but the schema pass
        # that would reject a malformed one is not guaranteed to have run
        # first -- filter to dicts rather than let a non-object raise
        # `AttributeError` here (same shape as
        # `_check_soc_vela_memory_profile`).
        variants = [v for v in (doc.get("variants") or []) if isinstance(v, dict)]
        for i, v in enumerate(variants):
            package = v.get("package") or ""
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


def _check_npu_ops_semantics(npu_ops_files) -> list:
    """Cross-checks on `metadata/npu_ops/<backend_family>/*.json` beyond pure
    schema validation (ADR-0028, reshaped from the flat one-file-per-backend
    layout to one-file-per-SUPPORT-TABLE-IDENTITY).

    The schema enforces per-file shape, but not facts that only exist
    relative to the file's PATH (its parent directory + its own filename):

      1. `applies_to.variant` + `applies_to.toolchain` +
         `applies_to.toolchain_version` must reproduce the filename exactly
         (`<variant>@<toolchain>-<toolchain_version>.json`).  Without this, a
         file could claim one identity in its path and another inside its own
         body, and a future consumer resolving a table by path alone would
         silently load metadata that disagrees with what it asked for.
      2. `op_namespace` must match the backend FAMILY's compiler ingest
         format -- the `ethos_u/` directory is TFLite (Vela), the `drpai/`
         directory is ONNX (DRP-AI Translator) -- mirroring each adapter's
         `accepts(src_format)`.  Scoring a model's ops against a list in the
         wrong vocabulary matches nothing and yields a categorically wrong
         no-fit verdict.
      3. `provenance.count_expected`, when present, must equal
         `len(supported_ops)` -- it exists specifically so a transcription
         that silently drops or duplicates an op (the exact defect this data
         asset was reshaped to correct) is caught mechanically rather than
         trusted on review alone.
      4. Every entry in `supported_ops` must itself be spelled in the
         vocabulary `op_namespace` declares -- TFLite builtins are
         UPPER_SNAKE (`CONV_2D`), ONNX operators are CamelCase or a short
         all-caps acronym and never contain an underscore (`Conv`, `LRN`).
         Check (2) above only catches a table in the wrong FILE (`onnx`
         table under `ethos_u/`); this catches a table with the wrong
         `op_namespace` LABEL for its own contents (an `onnx` table whose
         ops are actually spelled `CONV_2D`-style) -- the exact defect class
         this data asset exists to correct, and the schema's own
         `supported_ops[].pattern` admits both spellings so it can't tell
         them apart on its own.

    Returns a failure list shaped like `_check_files()`.
    """
    # Backend-FAMILY (the directory under metadata/npu_ops/) -> the source
    # format its compiler ingests.  A family with no entry here is unknown
    # territory for this cross-check (nothing to compare against), not a
    # failure -- new families are free to be added; this dict just doesn't
    # yet know their ingest format.
    _expected_namespace_by_family = {"ethos_u": "tflite", "drpai": "onnx"}
    failures: list[tuple[Path, list[str]]] = []
    for path in npu_ops_files:
        rel = path.relative_to(REPO).as_posix()
        try:
            doc = strict_json_loads(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # parse errors already reported by the schema pass
        if not isinstance(doc, dict):
            continue

        msgs: list[str] = []
        family = path.parent.name
        applies_to = doc.get("applies_to") if isinstance(doc.get("applies_to"), dict) else {}

        variant = applies_to.get("variant")
        toolchain = applies_to.get("toolchain")
        toolchain_version = applies_to.get("toolchain_version")
        if isinstance(variant, str) and isinstance(toolchain, str) and isinstance(toolchain_version, str):
            expected_stem = f"{variant}@{toolchain}-{toolchain_version}"
            if path.stem != expected_stem:
                msgs.append(
                    f"applies_to (variant={variant!r}, toolchain={toolchain!r}, "
                    f"toolchain_version={toolchain_version!r}) implies filename "
                    f"`{expected_stem}.json`, but this file is `{path.name}` -- "
                    f"a consumer resolving this table by path would load metadata "
                    f"that disagrees with what it asked for")

        namespace = doc.get("op_namespace")
        expected = _expected_namespace_by_family.get(family)
        if expected is not None and namespace != expected:
            msgs.append(
                f"op_namespace: `{namespace}` but the `{family}/` directory's "
                f"backend ingests `{expected}` -- see the matching adapter's "
                f"accepts(src_format)")

        # Spelling-vs-namespace (docstring item 4): TFLite builtins are
        # UPPER_SNAKE; ONNX operators are CamelCase or a short all-caps
        # acronym (`LRN`, `GRU`, `LSTM`) and never contain an underscore.
        # `op == op.upper()` is NOT the discriminator here -- it would
        # reject those legitimate all-caps ONNX acronyms as if they were
        # TFLite spellings. An underscore is what TFLite-style multi-word
        # names carry that ONNX names never do, so that is what a
        # wrong-vocabulary onnx table (`CONV_2D` instead of `Conv`) trips.
        ops = doc.get("supported_ops")
        if isinstance(ops, list):
            if namespace == "tflite":
                bad_ops = [op for op in ops
                          if not (isinstance(op, str) and op == op.upper())]
            elif namespace == "onnx":
                bad_ops = [op for op in ops if isinstance(op, str) and "_" in op]
            else:
                bad_ops = []
            if bad_ops:
                msgs.append(
                    f"op_namespace: `{namespace}` but supported_ops contains "
                    f"{len(bad_ops)} op(s) spelled in the wrong vocabulary: "
                    f"{bad_ops} -- TFLite builtins are UPPER_SNAKE, ONNX "
                    f"operators are CamelCase or a short all-caps acronym "
                    f"with no underscore")

        authority = doc.get("authority")
        has_banner = isinstance(doc.get("_generated"), str)
        if authority == "tool-generated" and not has_banner:
            msgs.append(
                "authority: tool-generated but no `_generated` DO-NOT-EDIT "
                "banner -- a machine-reproducible table should self-identify "
                "so a hand-edit is recognisable as wrong on sight")
        if authority == "vendor-manual" and has_banner:
            msgs.append(
                "authority: vendor-manual but carries a `_generated` "
                "DO-NOT-EDIT banner -- there is no script to regenerate a "
                "hand-transcribed table from, so the banner is misleading")

        provenance = doc.get("provenance") if isinstance(doc.get("provenance"), dict) else {}
        count_expected = provenance.get("count_expected")
        if isinstance(count_expected, int) and isinstance(ops, list) and len(ops) != count_expected:
            msgs.append(
                f"provenance.count_expected={count_expected} but supported_ops "
                f"has {len(ops)} entries -- a dropped/duplicated op vs. the "
                f"cited source, or a stale count_expected")

        if msgs:
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
            failures.append((rel, msgs))
    return failures


def _npu_backend(npu_type: str, subtype: str) -> str | None:
    """Map a SoC `npus[].type`/`.subtype` pair onto an inference backend id.

    Deliberately the SAME mapping as `tan.model.targets._npu_backend`, because
    both sides must agree on what `metadata/socs/**/*.json` means: alp-sdk
    validates that a perf point names a target the SKU has, and tan resolves
    the targets it compiles for.  Neither repo can import the other, so the
    defence against drift is that both read the SAME source of truth (the
    SoC's own `npus[]`) rather than a hand-kept backend<->SKU table, and that
    the derived set is pinned in `tests/scripts/test_model_perf_metadata.py`
    against the values `resolve_targets()` itself produces.

    Returns None for an NPU no backend claims -- an unknown accelerator is
    not this function's to guess at.
    """
    if npu_type.startswith("ethos-u"):
        return "ethos_u"
    if "drp" in npu_type or "drp" in subtype:      # renesas ("drp-ai" / "ai-mac+drp")
        return "drpai"
    if npu_type.startswith("dx") or "deepx" in npu_type:
        return "deepx_dxm1"
    return None


def _soc_perf_targets(soc: dict) -> dict[tuple[str, str], str | None]:
    """`(backend, accel_config)` -> declared `paired_core`, from one SoC's `npus[]`.

    `accel_config` is vela's `--accelerator-config` (`<type>-<mac_per_cycle>`)
    for Ethos-U and the empty string for every other backend, which has no
    such knob.  The KEY set mirrors `tan.model.targets._soc_targets`.

    The value is the `npus[].paired_core` the spec declares, or None when it
    declares none -- and None is a real answer, not a placeholder.  The Alif E8
    pairs each Ethos-U55 to a specific M55 (`m55_hp` / `m55_he`) and pairs the
    Ethos-U85 to nothing, so a caller may only enforce a pairing where one is
    written down.  Two `npus[]` entries that collapse onto the same
    `(backend, accel_config)` with DIFFERENT `paired_core` values make the
    pairing ambiguous, and ambiguity resolves to None: the metadata does not
    know, so nothing downstream may act as if it does.
    """
    out: dict[tuple[str, str], str | None] = {}
    for npu in soc.get("npus") or []:
        if not isinstance(npu, dict):
            continue
        npu_type = str(npu.get("type", ""))
        backend = _npu_backend(npu_type, str(npu.get("subtype", "")))
        if backend is None:
            continue
        accel = (f"{npu_type}-{npu['mac_per_cycle']}"
                 if backend == "ethos_u" and npu.get("mac_per_cycle") else "")
        paired = npu.get("paired_core")
        paired = paired if isinstance(paired, str) and paired else None
        key = (backend, accel)
        if key in out and out[key] != paired:
            out[key] = None
        else:
            out[key] = paired
    return out


def _load_som_preset(sku: str, metadata_root: Path) -> dict:
    """The SoM preset for a SKU, or `LookupError`.

    FAILS CLOSED for every caller below: they all state "this SKU does not
    have that <thing>" as a hard failure, so an empty or partial answer built
    from a file we could not read would reject a legitimate point.
    """
    preset_path = metadata_root / "e1m_modules" / f"{sku}.yaml"
    if not preset_path.is_file():
        raise LookupError(f"no SoM preset at {preset_path.name}")
    try:
        preset = strict_yaml_load(preset_path.read_text(encoding="utf-8"),
                                  source=preset_path)
    except Exception as exc:                                # pragma: no cover
        raise LookupError(f"{preset_path.name} does not parse: {exc}") from exc
    if not isinstance(preset, dict):                        # pragma: no cover
        raise LookupError(f"{preset_path.name} does not parse to a mapping")
    return preset


def _resolve_perf_cores(sku: str, metadata_root: Path) -> set[str]:
    """Every core id a SoM SKU declares, i.e. the keys of its `topology:` map.

    `E1M-AEN801` -> {`a32_cluster`, `m55_hp`, `m55_he`};
    `E1M-V2N101` -> {`a55_cluster`, `m33_sm`}.  A perf point names the core
    that drove the inference because the core changes the number outright --
    an A-cluster and an M-class CPU inference of the same model are not the
    same measurement, and without the core every `backend: "cpu"` point on one
    SKU would resolve to a single filename.

    Raises `LookupError` on an unresolvable SKU, for the same fail-closed
    reason as `_resolve_perf_targets`.
    """
    topology = _load_som_preset(sku, metadata_root).get("topology")
    if not isinstance(topology, dict) or not topology:
        raise LookupError(f"{sku}.yaml declares no `topology:` cores")
    return set(topology)


def _resolve_host_soc(sku: str, metadata_root: Path) -> dict:
    """The SKU's HOST SoC spec -- the `silicon:` ref its preset names -- parsed.

    Factored out of `_perf_target_map` so `_soc_npu_toolchain_names` can share
    the same preset -> `silicon:` -> SoC-spec resolution instead of a second,
    hand-copied walk of the same three files that could silently drift from
    this one.

    Raises `LookupError` when the SKU's preset or its `silicon:` SoC spec
    cannot be resolved.  FAILS CLOSED, never partial.
    """
    preset = _load_som_preset(sku, metadata_root)
    preset_name = f"{sku}.yaml"

    silicon = str(preset.get("silicon", ""))
    soc_path = resolve_soc_path(silicon, metadata_root)
    if soc_path is None:
        raise LookupError(f"malformed `silicon:` ref {silicon!r} in {preset_name}")
    if not soc_path.is_file():
        raise LookupError(f"no SoC spec for {silicon} at {soc_path}")
    return strict_json_loads(soc_path.read_text(encoding="utf-8"), source=soc_path)


def _soc_npu_toolchain_names(sku: str, metadata_root: Path) -> set[str]:
    """The toolchain names the SKU's HOST SoC spec's `npu_toolchain` block
    declares -- today always a subset of `{"vela"}`, since `npu_toolchain` is
    only ever written for an Ethos-U part (`_check_soc_vela_memory_profile`
    enforces that pairing on the SoC spec itself).  Empty when the SoC
    declares no `npu_toolchain` block at all, OR when it declares one that is
    not a mapping (guarded rather than left to raise `AttributeError` here --
    the same shape as `_check_soc_vela_memory_profile`'s `vela` lookup, and
    the schema pass that would reject the malformed shape runs separately).

    Raises `LookupError` for the same reason `_resolve_host_soc` does.
    """
    host = _resolve_host_soc(sku, metadata_root)
    npu_toolchain = host.get("npu_toolchain")
    npu_toolchain = npu_toolchain if isinstance(npu_toolchain, dict) else {}
    return set(npu_toolchain.keys())


def _perf_target_map(sku: str, metadata_root: Path) -> dict[tuple[str, str], str | None]:
    """`(backend, accel_config)` -> declared `paired_core`, for one SoM SKU.

    Host SoC `npus[]` + every OTHER SoC spec whose `variants[].alp_module_skus`
    lists this SKU (an on-module discrete accelerator -- the DEEPX DX-M1 on the
    V2M SKUs) + `("cpu", "")`, which is always present and pairs to no
    particular core.  Same derivation, off the same files, as
    `tan.model.targets.resolve_targets`.

    Raises `LookupError` when the SKU's preset or its `silicon:` SoC spec
    cannot be resolved.  FAILS CLOSED, never partial.
    """
    host = _resolve_host_soc(sku, metadata_root)
    targets = _soc_perf_targets(host)
    host_ref = host.get("ref")
    for path in sorted((metadata_root / "socs").glob("**/*.json")):
        soc = strict_json_loads(path.read_text(encoding="utf-8"), source=path)
        ref = soc.get("ref")
        if not ref or ref == host_ref:
            continue
        skus = {s for v in (soc.get("variants") or [])
                for s in (v.get("alp_module_skus") or [])}
        if sku in skus:
            for key, paired in _soc_perf_targets(soc).items():
                if key in targets and targets[key] != paired:
                    targets[key] = None
                else:
                    targets[key] = paired
    targets.setdefault(("cpu", ""), None)
    return targets


def _resolve_perf_targets(sku: str, metadata_root: Path) -> set[tuple[str, str]]:
    """Every `(backend, accel_config)` a SoM SKU actually resolves.

    The key set of `_perf_target_map`; see it for the derivation and the
    fail-closed contract.
    """
    return set(_perf_target_map(sku, metadata_root))


#: The stores a `<store>:<path>` citation may legitimately name.  This
#: allowlists the STORE SEGMENT itself, not the character class in front of
#: the colon: the previous expression (`^[A-Za-z0-9][A-Za-z0-9._+-]*:\S`)
#: accepted ANY colon-bearing string as a citation, so `todo:findit`,
#: `x:y`, `ask:Caner` and `note:see the log` all routed down the citation
#: branch and skipped reachability + the sha256/size_bytes re-hash a
#: repo-relative path gets -- the exact `see the log` / `ask Caner` / `n/a`
#: shapes `metadata/schemas/model-perf-v1.schema.json`'s `capture.reference`
#: names as what the citation allowlist replaced a denylist to keep out,
#: still let through the moment a colon follows them.  Defined before
#: `_LOCAL_PATH_REFERENCE` because that pattern's drive-letter alternative
#: needs it too.  The bare names, not the full citation prefixes: those are
#: `_STORE_CITATION_PREFIXES` below, derived from this single list so the
#: two never carry a different store count.
_STORE_NAMES = ("alp-sdk-internal", "https", "http")

#: The separator that follows each `_STORE_NAMES` entry in a legitimate
#: citation.  `alp-sdk-internal` is not URL-shaped, so its citation is
#: `<name>:<path>` -- a bare colon.  `https`/`http` ARE URL-shaped, so
#: requiring their own scheme separator (`://`) is what refuses
#: `https:findit` and `http:x`: a colon with no authority slashes names an
#: allowlisted SCHEME but is not a URL (the schema's own `capture.reference`
#: example is `https://example.org/...`, never `https:...`), yet it used to
#: satisfy the store-segment allowlist all the same, taking `model.source`'s
#: reachability + sha256/size_bytes re-hash off for a citation that
#: resolves for nobody -- see changelog.d/1520.md.  Every `_STORE_NAMES`
#: entry MUST have one; `test_store_citation_prefixes_cover_every_store_name`
#: enforces that a name added to one is not forgotten in the other.
_STORE_SEPARATORS = {"alp-sdk-internal": ":", "https": "://", "http": "://"}

#: `capture.reference` / `model.source` shapes that name a path on ONE
#: developer's machine rather than citing a store every reader can resolve.  A
#: public repo must never carry them (see the repo-wide "no local paths" rule);
#: a citation that resolves for nobody else also makes the point
#: unreproducible, which is the only thing a bench measurement is worth.
#:
#: The drive-letter alternative is discriminated on the SEPARATOR, not a word
#: boundary in front of the letter.  An earlier shape,
#: `(?:^|[^A-Za-z0-9])[A-Za-z]:[/\\]`, treated a single letter followed by
#: `:/` ANYWHERE in the string as a drive path -- which fires on the `a` in
#: `https://example.org/a:/b.log`'s path, the `c` in
#: `.../bench/c:/run.log`, the `a` in `?q=a:/b`, and the `b` in
#: `alp-sdk-internal:a/b:/c.log`, refusing every one of those legitimate
#: citations because a URL path or query is free to contain a bare
#: single-letter segment before a `:/`.  The two separators do not carry
#: that risk equally, so they are no longer treated alike:
#:
#:   * `[A-Za-z]:\` (backslash) is checked ANYWHERE in the string.  A
#:     backslash is not a legal URL character and never appears mid-path or
#:     mid-query in a real citation, so `https:C:\Users\user\log.txt` and
#:     `http:D:\bench\run.log` -- a Windows drive path tacked on AFTER a
#:     legitimate-looking store -- are still caught wherever the drive
#:     letter lands.
#:   * `[A-Za-z]:/` (forward slash) is checked ONLY at the string start, or
#:     immediately after one of `_STORE_NAMES`'s colon (`https:C:/...`,
#:     `alp-sdk-internal:C:/...`) -- the two positions a drive letter can
#:     legitimately open a reference.  Anywhere else the same three
#:     characters are an ordinary URL path or query segment, not a drive
#:     letter, and a real `https://` / `http://` URL's `//` never matches
#:     either position (the character right after the store colon is `/`,
#:     not a letter).
_LOCAL_PATH_REFERENCE = re.compile(
    r"^[/\\]"
    r"|[A-Za-z]:\\"
    r"|^[A-Za-z]:/"
    r"|^(?:" + "|".join(_STORE_NAMES) + r"):[A-Za-z]:/"
    r"|onedrive",
    re.IGNORECASE)

#: `alp-sdk-internal:models/person_detect_int8.tflite`,
#: `https://example.org/zoo/x.tflite`.  Derived from `_STORE_NAMES` +
#: `_STORE_SEPARATORS`, never a second hand-typed store list, so the store
#: count cannot drift between the two.  Used to tell the two legal
#: `model.source` shapes apart; `_LOCAL_PATH_REFERENCE` is applied FIRST, so
#: a `C:\...` never reaches this and gets read as a store named `C`.
_STORE_CITATION_PREFIXES = tuple(
    name + _STORE_SEPARATORS[name] for name in _STORE_NAMES)

#: MUST stay byte-identical to `capture.reference`'s `pattern` in
#: metadata/schemas/model-perf-v1.schema.json -- JSON Schema has no way to
#: `$ref` a Python constant, so the two are kept in lockstep by
#: `test_model_source_and_capture_reference_agree_on_the_store_allowlist`
#: instead, which fails the moment one is edited without the other.
#: Joined WITHOUT `re.escape`: none of the three prefixes contain a regex
#: metacharacter (a `-` needs escaping only INSIDE a character class, and
#: `:` / `/` are not metacharacters at all), and `re.escape` used to escape
#: the bare store names anyway, producing `alp\-sdk\-internal` -- legal for
#: Python's `re` (Annex-B leniency lets `\-` mean a literal `-` outside a
#: class) but an invalid escape under ECMA-262 `u`/`v` mode, which is what
#: Ajv (and any JS/TS JSON Schema consumer, e.g. alp-sdk-vscode) compiles a
#: `pattern` string under by default (`unicodeRegExp: true`) -- so the
#: shipped schema could not be compiled by a JavaScript consumer at all.
#: `test_store_citation_pattern_has_no_non_ecma262_escape` guards this.
_STORE_CITATION = re.compile(
    r"^(?:" + "|".join(_STORE_CITATION_PREFIXES) + r")\S")

#: The bench recipe's timed-run floor (docs/bench/model-perf-capture.md §4).
#: 100 timed runs after >= 10 discarded warm-ups, so `latency_ms_p95` is the
#: 95th percentile of at least a hundred samples rather than an interpolation
#: across a handful.
_MIN_TIMED_RUNS = 100


def _toolchain_profile_digest(toolchain: dict) -> str:
    """The 12-hex digest of a perf point's toolchain PROFILE.

    The profile is every key under `toolchain` OTHER than `name` and `version`
    -- today `system_config`, `memory_mode` and `pins` -- canonicalised as JSON
    with sorted keys and no whitespace, sha256'd, truncated to 12.  Derived as
    "everything except name and version" rather than from a hard-coded key list
    so that a profile key added to the schema later enters the identity
    automatically instead of silently sharing a filename with the points that
    predate it.

    It is in the filename because it changes the number: `Ethos_U85_SRAM_Only`
    and `Ethos_U85_SYS_DRAM_Mid` are different machines (the second is
    DRAM-backed and this part has no DRAM), and two points measured under them
    would otherwise resolve to one path, where the survivor is whichever was
    written last.  It is deliberately NOT part of the consumer match key: a
    customer holding no toolchain cannot state a profile.
    """
    profile = {k: v for k, v in toolchain.items() if k not in ("name", "version")}
    canonical = json.dumps(profile, sort_keys=True, separators=(",", ":"),
                           ensure_ascii=False)
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()[:12]


def _check_model_perf_semantics(perf_files) -> list:
    """Cross-checks on bench-measured model perf points beyond pure schema
    validation, mirroring `_check_npu_ops_semantics` for the same reasons.

    A perf point is the one data asset in this tree that a customer reads as
    an EXACT answer about their own hardware, taken on our authority, with no
    toolchain and no board of their own to check it against.  Everything below
    exists so a point cannot quietly stop describing the thing that produced
    it:

      1. The PATH is a claim -- `<sku>/<target>/<slug>-<sha12>@<toolchain>-
         <version>+<hw_rev>+<core>+<profile12>.json` -- and the body must
         reproduce every segment exactly.  `<target>` is `accel_config` when
         the backend has one and `backend` when it does not; `<profile12>` is
         `_toolchain_profile_digest`.  EVERY SEGMENT AFTER THE SLUG IS THERE
         BECAUSE IT CHANGES THE NUMBER, and a segment left out is a segment on
         which the second measurement silently overwrites the first: the model
         sha (a re-bench of changed bytes must ACCUMULATE a second point --
         the first is still right for a customer holding the old bytes), the
         hardware revision, the core, and the toolchain profile.
      2. `measured_on.sku` must exist under `metadata/e1m_modules/`.
      3. `(measured_on.backend, measured_on.accel_config)` must be a target
         that SKU actually resolves, so a point cannot claim silicon the
         module does not carry.
      4. `measured_on.hw_rev` must be a key in that SoM family's
         `hw-revisions.yaml`.  Existence only, deliberately: a point measured
         on a `reserved` or pre-production revision is still a real
         measurement of that revision.
      5. `measured_on.core` must be a core that SKU's `topology:` declares,
         and -- only where the SoC spec declares a `paired_core` for the
         matched NPU -- must be that core.  Where the spec declares no pairing
         (the Alif E8's Ethos-U85 today) nothing is inferred: the metadata does
         not know, so the gate does not guess.
      6. An `ethos_u` point must record BOTH `toolchain.system_config` and
         `toolchain.memory_mode`.  Invoked with neither flag, `ethos-u-vela`
         5.1.0 silently picks a default per accelerator -- on the U85 that is
         `Ethos_U85_SYS_DRAM_Mid` / `Dedicated_Sram_384KB`, a DRAM-backed
         profile on a part whose `external_memory_interfaces` declares no
         DRAM.  A point captured that way is exactly measured and describes a
         machine the module is not; recording the profile is what lets a
         reader tell the two apart.  Its `toolchain.name` must also be one the
         SoC spec's own `npu_toolchain` block names (today always `vela`) --
         `toolchain.name` is one of the eight consumer match-key fields, so an
         `ethos_u` point naming, say, `dxcom` is not cosmetic: it makes the
         point unmatchable, or matchable by the wrong consumer.  This
         `toolchain.name` cross-check is `ethos_u`-only, for two DIFFERENT
         reasons on the two kinds of backend it excludes -- they are not the
         same claim and must not be stated as one.  `drpai` and `deepx_dxm1`
         are excluded because their host SoC specs
         (`metadata/socs/renesas/rzv2n/n44.json`,
         `metadata/socs/deepx/dx/m1.json`) declare no `npu_toolchain` block
         at all, since that block is written only for an Ethos-U part, so
         there is nothing to cross-check `toolchain.name` against.  `cpu` is
         excluded for a DIFFERENT reason: every Alif Ensemble and NXP i.MX93
         host SoC spec a `cpu` target resolves to (`alif:ensemble:e3`..`e8`,
         `nxp:imx9:imx93`) DOES declare `npu_toolchain.vela` -- a CPU point
         simply has no accelerator toolchain to check `toolchain.name`
         against in the first place, which is a fact about the backend
         itself, not about whether its host SoC spec happens to carry the
         block.  A SoC that resolves as `ethos_u` yet declares no
         `npu_toolchain` block is a refusal, not a skip, matching this
         function's other fail-closed rules -- unreachable while every
         shipping Ethos-U SoC spec's own `npu_toolchain.vela` block is itself
         enforced present by `_check_soc_vela_memory_profile`, but not
         provably so from this function alone.
      7. `measured.latency_ms_p95` must be >= `measured.latency_ms_mean`.  A
         p95 below the mean is not a tighter number, it is two runs' figures
         pasted into one point.
      8. `measured.req_sram_kib * 1024` must be >= `measured.arena_bytes` when
         both are present.  The on-device selector's fit test is
         `e->arena_sram_kib == 0u || t->req_sram_kib <= e->arena_sram_kib`
         (`src/backends/inference/alp_model_select.c`), so a footprint that
         undercuts the arena the same compile reported -- a zero above all --
         fits every arena on every engine and turns the fit gate into a check
         that cannot fail.  This is the defect the tier exists to close, so it
         is enforced rather than described.
      9. `measured.runs` must be >= `_MIN_TIMED_RUNS` when latency is present.
         The recipe's floor; a `runs: 1` point whose mean and p95 are the same
         single number is a single shot wearing a measurement's clothes.
     10. `capture.date` must PARSE, not merely match the ISO shape:
         `2026-13-45` satisfies the pattern and is not a day.
     11. A `backend: "cpu"` point may not report a nonzero `measured.npu_ops`.
         There is no NPU on that path to place an operator on, so a figure
         there came from another run's report and everything beside it is
         suspect.
     12. No file under `metadata/model_perf/` may carry `_fixture`.  That key
         marks the synthetic documents under `tests/fixtures/model_perf/`
         whose `measured` values are placeholders; the published tree must be
         incapable of absorbing one.
     13. `capture.reference` and `model.source` must cite a store, not a local
         filesystem path.

    Reading `model.source` back -- re-hashing the in-repo model file and
    requiring it to equal `model.sha256` -- deliberately does NOT live here.
    That path points into `tests/fixtures/`, which does not exist in the
    metadata-ONLY scratch clone `tests/scripts/test_alp_cli_new_som.py`'s
    `_clone_metadata_gates` runs this script against, so a copy here could
    only be a silent skip.  It lives in
    `tests/scripts/test_model_perf_metadata.py`, which always runs against the
    real checkout -- the same split, for the same reason, as
    `_check_soc_vela_memory_profile`'s `source` citations.

    Returns a failure list shaped like `_check_files()`.
    """
    metadata_root = REPO / "metadata"
    published_root = metadata_root / "model_perf"
    failures: list[tuple[str, list[str]]] = []
    for path in perf_files:
        rel = path.relative_to(REPO).as_posix()
        try:
            doc = strict_json_loads(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # parse errors already reported by the schema pass
        if not isinstance(doc, dict):
            continue

        msgs: list[str] = []
        measured_on = doc.get("measured_on") if isinstance(doc.get("measured_on"), dict) else {}
        model = doc.get("model") if isinstance(doc.get("model"), dict) else {}
        toolchain = doc.get("toolchain") if isinstance(doc.get("toolchain"), dict) else {}
        measured = doc.get("measured") if isinstance(doc.get("measured"), dict) else {}
        capture = doc.get("capture") if isinstance(doc.get("capture"), dict) else {}

        sku = measured_on.get("sku")
        hw_rev = measured_on.get("hw_rev")
        core = measured_on.get("core")
        backend = measured_on.get("backend")
        accel = measured_on.get("accel_config")

        # (1) the path is a claim the body must reproduce.
        if isinstance(sku, str) and path.parent.parent.name != sku:
            msgs.append(
                f"measured_on.sku={sku!r} but this file sits under "
                f"`{path.parent.parent.name}/` -- a consumer resolving points "
                f"for a SKU by path would load a measurement taken on another "
                f"module")
        if isinstance(backend, str) and isinstance(accel, str):
            expected_dir = accel or backend
            if path.parent.name != expected_dir:
                msgs.append(
                    f"measured_on (backend={backend!r}, accel_config={accel!r}) "
                    f"implies directory `{expected_dir}/`, but this file sits "
                    f"under `{path.parent.name}/`")
        slug = model.get("slug")
        sha256 = model.get("sha256")
        tc_name = toolchain.get("name")
        tc_version = toolchain.get("version")
        if (isinstance(slug, str) and isinstance(sha256, str) and len(sha256) >= 12
                and isinstance(tc_name, str) and isinstance(tc_version, str)
                and isinstance(hw_rev, str) and isinstance(core, str)):
            profile12 = _toolchain_profile_digest(toolchain)
            expected_stem = (f"{slug}-{sha256[:12]}@{tc_name}-{tc_version}"
                             f"+{hw_rev}+{core}+{profile12}")
            if path.stem != expected_stem:
                msgs.append(
                    f"model (slug={slug!r}, sha256[:12]={sha256[:12]!r}) + "
                    f"toolchain (name={tc_name!r}, version={tc_version!r}, "
                    f"profile digest {profile12!r}) + measured_on "
                    f"(hw_rev={hw_rev!r}, core={core!r}) imply "
                    f"filename `{expected_stem}.json`, but this file is "
                    f"`{path.name}` -- the filename is the measurement identity, "
                    f"and every segment of it is one that changes the number, so "
                    f"a mismatch means one measurement is sitting where another "
                    f"belongs and the point that was there is gone")

        # (2)+(3)+(5) the SKU exists, it really has this target, and the core
        # that drove the inference is one the module declares.
        if isinstance(sku, str):
            try:
                target_map = _perf_target_map(sku, metadata_root)
                cores = _resolve_perf_cores(sku, metadata_root)
            except LookupError as exc:
                msgs.append(
                    f"measured_on.sku={sku!r}: cannot resolve this module's "
                    f"accelerator targets and cores ({exc}) -- refused rather "
                    f"than skipped, because skipping would accept a point "
                    f"naming any target at all")
            else:
                if isinstance(backend, str) and isinstance(accel, str):
                    if (backend, accel) not in target_map:
                        offered = ", ".join(
                            f"{b}/{a or '-'}" for b, a in sorted(target_map))
                        msgs.append(
                            f"measured_on (backend={backend!r}, "
                            f"accel_config={accel!r}) is not a target "
                            f"{sku} resolves -- it offers: {offered}")
                if isinstance(core, str):
                    if core not in cores:
                        msgs.append(
                            f"measured_on.core={core!r} is not a core {sku} "
                            f"declares -- its `topology:` names "
                            f"{sorted(cores)}. The core is part of the "
                            f"measurement identity because it changes the "
                            f"number, so a point cannot name one the module "
                            f"does not have")
                    paired = target_map.get((backend, accel)) \
                        if isinstance(backend, str) and isinstance(accel, str) else None
                    if paired and core != paired:
                        msgs.append(
                            f"measured_on.core={core!r} but the SoC spec pairs "
                            f"accelerator {accel or backend!r} to "
                            f"`{paired}` (`npus[].paired_core`) -- that "
                            f"accelerator cannot have been driven by this core, "
                            f"so either the core or the target is the wrong one")

        # (4) the hardware revision exists in the family table.
        if isinstance(sku, str) and isinstance(hw_rev, str):
            try:
                family_dir = _sku_family(sku)
            except ValueError:
                msgs.append(f"measured_on.sku={sku!r}: unrecognised SoM SKU pattern, "
                            f"so `hw_rev` cannot be checked against a family table")
            else:
                table = metadata_root / "e1m_modules" / family_dir / "hw-revisions.yaml"
                if not table.is_file():
                    msgs.append(
                        f"measured_on.hw_rev={hw_rev!r}: no "
                        f"metadata/e1m_modules/{family_dir}/hw-revisions.yaml to "
                        f"check it against")
                else:
                    revisions = strict_yaml_load(table.read_text(encoding="utf-8"),
                                                 source=table)
                    # `revisions` is a mapping in every valid hw-revisions.yaml,
                    # but the schema pass that would reject a malformed one
                    # (e.g. a bare list) is not guaranteed to have run first --
                    # guard rather than let a non-mapping raise `AttributeError`
                    # here (same shape as `_check_soc_vela_memory_profile`).
                    revisions = revisions if isinstance(revisions, dict) else {}
                    known = revisions.get("hw_revisions")
                    known = known if isinstance(known, dict) else {}
                    if hw_rev not in known:
                        msgs.append(
                            f"measured_on.hw_rev={hw_rev!r} is not a revision of the "
                            f"{family_dir} family -- "
                            f"metadata/e1m_modules/{family_dir}/hw-revisions.yaml "
                            f"declares {sorted(known) or '(none)'}")

        # (6) an Ethos-U point without its vela profile describes an unknown machine.
        if backend == "ethos_u":
            missing = [k for k in ("system_config", "memory_mode")
                       if not isinstance(toolchain.get(k), str) or not toolchain[k]]
            if missing:
                msgs.append(
                    f"backend `ethos_u` but toolchain records no "
                    f"{' and no '.join(missing)} -- invoked flagless, "
                    f"ethos-u-vela picks a default per accelerator "
                    f"(`Ethos_U85_SYS_DRAM_Mid` / `Dedicated_Sram_384KB` on the "
                    f"U85, which is DRAM-backed), so the arena figures would "
                    f"describe that profile and not this module")

            # toolchain.name is one of the eight consumer match-key fields
            # (measured_on.sku + hw_rev + core + backend + accel_config +
            # model.sha256 + toolchain.name + toolchain.version), so a name
            # that does not match the accelerator is not cosmetic -- it makes
            # the point unmatchable, or matchable by the wrong consumer.  The
            # SoC spec's own npu_toolchain block already names the only
            # toolchain that compiles for this accelerator.
            #
            # This is ETHOS_U ONLY, for two DIFFERENT reasons on the two
            # kinds of backend it excludes.  drpai/deepx_dxm1 are excluded
            # because their host SoC specs (metadata/socs/renesas/rzv2n/
            # n44.json, metadata/socs/deepx/dx/m1.json) declare no
            # `npu_toolchain` block at all; `npu_toolchain` is written only
            # for an Ethos-U part.  Adding that block to those SoC specs
            # would let this same cross-check run for those backends too.
            # cpu is excluded for a DIFFERENT reason: its host SoC specs
            # (every Alif Ensemble / NXP i.MX93 part) DO carry
            # `npu_toolchain.vela` -- a CPU point simply has no accelerator
            # toolchain to check `toolchain.name` against, a fact about the
            # backend rather than about the host SoC spec.
            if isinstance(sku, str) and isinstance(tc_name, str):
                try:
                    known_toolchains = _soc_npu_toolchain_names(sku, metadata_root)
                except LookupError as exc:
                    msgs.append(
                        f"measured_on.sku={sku!r}: cannot resolve this "
                        f"module's SoC spec to check toolchain.name against "
                        f"its npu_toolchain block ({exc})")
                else:
                    if not known_toolchains:
                        msgs.append(
                            f"measured_on.sku={sku!r}: backend `ethos_u` but "
                            f"this module's SoC spec declares no "
                            f"npu_toolchain block at all -- refused rather "
                            f"than skipped, because toolchain.name="
                            f"{tc_name!r} cannot be checked against a "
                            f"toolchain list that is not there, and this "
                            f"function's other rules already refuse an "
                            f"unresolvable SKU rather than pass over it")
                    elif tc_name not in known_toolchains:
                        msgs.append(
                            f"backend `ethos_u` but toolchain.name={tc_name!r} "
                            f"-- {sku}'s SoC spec's npu_toolchain block names "
                            f"{sorted(known_toolchains)}, and only that "
                            f"toolchain compiles for this accelerator")

        # (7) p95 below the mean is two runs pasted into one point.
        mean = measured.get("latency_ms_mean")
        p95 = measured.get("latency_ms_p95")
        if isinstance(mean, (int, float)) and isinstance(p95, (int, float)) and p95 < mean:
            msgs.append(
                f"measured.latency_ms_p95={p95} is below "
                f"measured.latency_ms_mean={mean} -- a 95th percentile cannot "
                f"undercut the mean of the same runs")

        # (8) a footprint that undercuts its own arena fits everything.
        #
        # The on-device selector's fit test is
        # `e->arena_sram_kib == 0u || t->req_sram_kib <= e->arena_sram_kib`
        # (src/backends/inference/alp_model_select.c), so a `req_sram_kib` of 0
        # -- or any figure below the arena the SAME compile reported -- passes
        # against every engine on every module.  Publishing one would re-open
        # the always-fits defect this tier exists to close, from the data side
        # instead of the code side, and it would do it wearing
        # `basis: "bench"` / `confidence: "certain"`.
        arena_bytes = measured.get("arena_bytes")
        req_sram_kib = measured.get("req_sram_kib")
        if (isinstance(arena_bytes, int) and not isinstance(arena_bytes, bool)
                and isinstance(req_sram_kib, int) and not isinstance(req_sram_kib, bool)
                and req_sram_kib * 1024 < arena_bytes):
            msgs.append(
                f"measured.req_sram_kib={req_sram_kib} is {req_sram_kib * 1024} "
                f"bytes, below measured.arena_bytes={arena_bytes} from the same "
                f"compile -- the on-device fit test is `req_sram_kib <= "
                f"arena_sram_kib` and treats 0 as `fits anything`, so a footprint "
                f"that does not even cover its own arena makes the fit gate "
                f"incapable of failing. A footprint that could not be measured is "
                f"OMITTED, never zero-filled")

        # (9) the recipe's timed-run floor.
        runs = measured.get("runs")
        if (isinstance(mean, (int, float))
                and isinstance(runs, int) and not isinstance(runs, bool)
                and runs < _MIN_TIMED_RUNS):
            msgs.append(
                f"measured.runs={runs} is below the {_MIN_TIMED_RUNS}-run floor "
                f"docs/bench/model-perf-capture.md §4 sets -- below it "
                f"`latency_ms_p95` is an interpolation across a handful of "
                f"samples rather than a percentile, and a point whose mean and "
                f"p95 are the same single number is a single shot wearing a "
                f"measurement's clothes")

        # (10) an ISO-shaped string is not necessarily a day: `2026-13-45`
        # satisfies the schema pattern.
        date = capture.get("date")
        if isinstance(date, str):
            try:
                datetime.date.fromisoformat(date)
            except ValueError:
                msgs.append(
                    f"capture.date={date!r} matches the ISO shape but is not a "
                    f"real calendar date -- a capture nobody can place in time "
                    f"cannot be correlated with the raw log it cites")

        # (11) a CPU point has no NPU to place an operator on.
        npu_ops = measured.get("npu_ops")
        if (backend == "cpu" and isinstance(npu_ops, int)
                and not isinstance(npu_ops, bool) and npu_ops != 0):
            msgs.append(
                f"backend `cpu` but measured.npu_ops={npu_ops} -- there is no "
                f"accelerator on this path to place an operator on, so that "
                f"figure came from another run's report and every figure beside "
                f"it is suspect")

        # (12) the published tree cannot absorb a synthetic fixture.
        if "_fixture" in doc and path.is_relative_to(published_root):
            msgs.append(
                "`_fixture` marks a synthetic document whose `measured` values "
                "are placeholders, not measurements -- it belongs under "
                "tests/fixtures/model_perf/ and must never ship under "
                "metadata/model_perf/, where a consumer would read it as bench "
                "data")

        # (13) citations, not somebody's disk -- for the capture AND for the
        # model bytes, which carry exactly the same leak and the same
        # resolves-for-nobody-else failure.
        source = model.get("source")
        for field, value in (("capture.reference", capture.get("reference")),
                             ("model.source", source)):
            if isinstance(value, str) and _LOCAL_PATH_REFERENCE.search(value):
                msgs.append(
                    f"{field}={value!r} looks like a path on one "
                    f"machine rather than a `<store>:<path>` citation or a "
                    f"repo-relative path -- this repo is public and such a "
                    f"reference resolves for nobody else, so the measurement "
                    f"stops being reproducible")
        # A `model.source` that is not a citation is a repo-relative path, and
        # a repo-relative path that climbs out of the checkout names a machine
        # as surely as `/home/...` does.  Re-hashing those bytes is the pytest
        # suite's job (they do not exist in the metadata-only scratch clone);
        # refusing the SHAPE needs no bytes and so belongs here.
        if (isinstance(source, str) and not _STORE_CITATION.match(source)
                and ".." in Path(source.replace("\\", "/")).parts):
            msgs.append(
                f"model.source={source!r} is not a `<store>:<path>` citation, "
                f"so it is read as a repo-relative path -- and it climbs out "
                f"of the checkout with `..`, which resolves somewhere different "
                f"on every machine")

        if msgs:
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
            failures.append((rel, msgs))
        else:
            print(f"OK   {rel}")
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
    # #1470: npu_toolchain.vela vs npus[] / external_memory_interfaces on the SAME spec.
    soc_failures += _check_soc_vela_memory_profile(soc_files)
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

    # Per-NPU op-support tables (the static-analyzer data asset, ADR-0028).
    # One file per SUPPORT-TABLE IDENTITY under a per-backend-family
    # subdirectory (metadata/npu_ops/<family>/<variant>@<toolchain>-
    # <toolchain_version>.json) -- glob with `**` so this ALSO catches a file
    # sitting directly under metadata/npu_ops/ (the retired flat layout).
    # `*/*.json` looked recursive but isn't: it requires exactly one
    # directory level, so a reintroduced flat file matches nothing and never
    # reaches schema/semantic validation at all -- silently, not as a FAIL.
    # A family directory can be legitimately absent (metadata/npu_ops/ has
    # no deepx/ -- dxcom publishes no op-support table; see
    # _check_npu_ops_semantics).
    npu_ops_failures: list = []
    npu_ops_files: list = []
    if NPU_OPS_SCHEMA.is_file():
        npu_ops_schema = json.loads(NPU_OPS_SCHEMA.read_text(encoding="utf-8"))
        npu_ops_validator = jsonschema.Draft202012Validator(npu_ops_schema)
        npu_ops_files = sorted(NPU_OPS.glob("**/*.json"))
        if npu_ops_files:
            print()
            npu_ops_failures = _check_files(
                "JSON", npu_ops_files, npu_ops_validator,
                lambda p: strict_json_loads(p.read_text(encoding="utf-8"), source=p),
                "op_namespace",
            )
            npu_ops_failures += _check_npu_ops_semantics(npu_ops_files)

    # Bench-measured model perf points (the tier-2 data asset).  One file per
    # MEASUREMENT IDENTITY under metadata/model_perf/<sku>/<target>/<slug>-
    # <sha12>@<toolchain>-<version>+<hw_rev>+<core>+<profile12>.json -- `**`
    # for the same reason npu_ops uses it: a file dropped at any other depth
    # must still reach the schema and semantic passes rather than silently
    # matching nothing.
    #
    # `metadata/model_perf/` is EMPTY until the first bench campaign, and an
    # empty glob makes every check below pass over nothing.  That vacuum is
    # closed in tests/scripts/test_model_perf_metadata.py, which drives this
    # same schema + `_check_model_perf_semantics` against a real fixture point
    # under tests/fixtures/model_perf/ and against a mutation of every rule.
    # Absence of a point is never a verdict: a model/SKU/target combination
    # with no file here is `undetermined`, not `does not fit`.
    model_perf_failures: list = []
    model_perf_files: list = sorted(MODEL_PERF.glob("**/*.json"))
    if not MODEL_PERF_SCHEMA.is_file():
        # The glob comes FIRST and this branch fails loudly, because the
        # obvious spelling -- wrapping the whole pass in `if
        # MODEL_PERF_SCHEMA.is_file()` -- makes deleting or renaming the
        # schema turn every published point into an unchecked one at rc=0.
        # A point broken thirteen ways would then validate silently, and this is
        # the one data asset a customer reads as an exact answer about their
        # own hardware.  No schema plus no points is legitimately nothing to
        # check; no schema WITH points is a gate that has been removed.
        if model_perf_files:
            print()
            print(f"FAIL metadata/schemas/{MODEL_PERF_SCHEMA.name}")
            print(f"  · missing, but {len(model_perf_files)} perf point(s) are "
                  f"published under metadata/model_perf/ -- they would be "
                  f"accepted unchecked")
            model_perf_failures = [(f"metadata/schemas/{MODEL_PERF_SCHEMA.name}",
                                    ["schema missing while perf points ship"])]
    else:
        model_perf_schema = json.loads(MODEL_PERF_SCHEMA.read_text(encoding="utf-8"))
        model_perf_validator = jsonschema.Draft202012Validator(model_perf_schema)
        if model_perf_files:
            print()
            model_perf_failures = _check_files(
                "JSON", model_perf_files, model_perf_validator,
                lambda p: strict_json_loads(p.read_text(encoding="utf-8"), source=p),
                "stance",
            )
            model_perf_failures += _check_model_perf_semantics(model_perf_files)

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

    # SoM `memory:` <-> `on_module` population cross-check.
    memory_population_failures: list = []
    if som_files:
        print()
        memory_population_failures = _check_som_memory_population(som_files)

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
                      + len(npu_ops_failures)
                      + len(model_perf_failures)
                      + len(library_failures) + len(library_semantic_failures)
                      + len(board_target_failures)
                      + len(restriction_failures)
                      + len(instance_uniqueness_failures)
                      + len(slot0_address_failures)
                      + len(memory_population_failures)
                      + len(silicon_kconfig_failures)
                      + len(peripheral_kconfig_failures)
                      + len(tier_a_library_ci_failures))
    print(f"{len(soc_files)} SoC file(s) + {len(som_files)} SoM preset(s) + "
          f"{len(hwrev_files)} hw-revisions file(s) + "
          f"{len(board_files)} board preset(s) + {len(chip_files)} chip file(s) + "
          f"{len(block_files)} block file(s) + {len(npu_ops_files)} npu-ops file(s) + "
          f"{len(model_perf_files)} model-perf point(s) + "
          f"{len(library_files)} library manifest(s) + Kconfig registries + "
          f"tier-a-library-ci registry "
          f"checked, {total_failures} failure(s)")
    return 0 if total_failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
