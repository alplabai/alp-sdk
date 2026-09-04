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
from alp_model.targets import resolve_targets, _npu_backend, _accel_config  # noqa: E402
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
# The V2N/V2M on-module GD32G553 supervisor pin-wiring source
# scripts/gen_zephyr_board.py's `_v2n_pinctrl_dtsi()` / `_v2n_defconfig()`
# read (#655).  There is NO auto-discovery in this script -- an
# unregistered schema is silently unvalidated -- so this constant pair is
# load-bearing, not decorative.
SUPERVISOR_LINKS_SCHEMA = REPO / "metadata" / "schemas" / "supervisor-links-v1.schema.json"
SUPERVISOR_LINKS_DATA = REPO / "metadata" / "e1m_modules" / "v2n" / "supervisor-links.yaml"
BLOCK_SCHEMA = REPO / "metadata" / "schemas" / "block-v1.schema.json"
BLOCKS = REPO / "metadata" / "blocks"
NPU_OPS_SCHEMA = REPO / "metadata" / "schemas" / "npu-ops-v1.schema.json"
NPU_OPS = REPO / "metadata" / "npu_ops"
MODEL_ZOO_SCHEMA = REPO / "metadata" / "schemas" / "model-zoo-v1.schema.json"
MODEL_ZOO = REPO / "metadata" / "model_zoo"
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


def _check_som_i2c_address_collisions(som_files) -> list:
    """Reject two on-module I2C devices sharing (bus, address_7bit).

    `on_module.i2c_devices.<bus>.devices[]` records the schematic
    strap-selected address per on-module device.  Two chips answering the
    same address on the same bus is a real silicon defect, not an
    editorial nit: #1163 (TMP112 vs the DEEPX LPDDR buck, both at 0x48)
    and #1659 (an INA236 vs the TAS2563 broadcast address, also 0x48) are
    real prior instances (#1845).  JSON Schema has no way to express
    "unique across sibling array entries by a derived key", so enforce it
    here.

    An entry does NOT count as a fixed, collision-checkable address when:
      * `address_7bit` is the literal `"TBD"` -- pending the HW-config
        writeup, not yet a real value;
      * `address_7bit` is the literal `"configurable"` -- picked by the
        chip's own firmware (e.g. the GD32 supervisor MCU), not a
        hardware-fixed strap two devices could physically contend over;
      * `assembled: false` -- DNI, physically absent from the bus;
      * `broadcast_address: true` -- a broadcast/global-call address
        legitimately shared by design (e.g. TAS2563's 0x48, see
        metadata/chips/tas2563.yaml). Do not reach for this opt-out to
        silence a real strap conflict.

    Returns a failure list shaped like `_check_files()`.
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
        buses = _as_dict(_as_dict(doc.get("on_module")).get("i2c_devices"))

        msgs: list[str] = []
        checked = 0
        for bus_name, bus in sorted(buses.items()):
            if not isinstance(bus, dict):
                continue
            seen: dict[int, list[dict]] = {}
            for dev in _dict_entries(bus.get("devices")):
                if dev.get("assembled") is False or dev.get("broadcast_address") is True:
                    continue
                addr = dev.get("address_7bit")
                if not isinstance(addr, str) or not re.fullmatch(r"0x[0-9A-Fa-f]{1,2}", addr):
                    continue  # "TBD" / "configurable" / malformed -- not a fixed address
                checked += 1
                seen.setdefault(int(addr, 16), []).append(dev)
            for addr_int, devs in sorted(seen.items()):
                if len(devs) < 2:
                    continue
                names = ", ".join(f"{d.get('chip')}/{d.get('role')}" for d in devs)
                msgs.append(
                    f"on_module.i2c_devices.{bus_name}: {names} all declare "
                    f"address_7bit=0x{addr_int:02X} on the same bus -- two "
                    f"devices cannot share a fixed I2C address (#1845); set "
                    f"broadcast_address: true only if this is a real "
                    f"broadcast/global-call address")

        if msgs:
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
            failures.append((rel, msgs))
        else:
            print(f"OK   {rel}  (i2c_devices: {checked} address(es) checked, "
                  f"no collisions)")
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


def _check_board_i2c_address_collisions(board_files) -> list:
    """Reject two on-board I2C device instances sharing an address.

    A board preset declares on-board I2C devices two ways, checked
    separately here because each carries its own bus scope:

      * `i2c_devices[]` -- ONE array per file; every entry sits on the
        single implicit on-board I2C bus documented in-file (e.g.
        e1m-evk.yaml: "all on ALP_E1M_I2C0, the sensor bus"). The schema
        has no per-entry bus field because the board only has the one.
      * `audio.codecs[]` -- each entry names its own `i2c_bus` explicitly
        (a board can carry more than one audio-adjacent bus).

    Two chips answering the same address on the same bus is a real
    silicon defect, not an editorial nit: #1163 (TMP112 vs the DEEPX
    LPDDR buck, both at 0x48) and #1659 (an INA236 vs the TAS2563
    broadcast address, also 0x48) are real prior instances (#1845). JSON
    Schema has no way to express "unique across sibling array entries by
    a derived key", so enforce it here. An entry with
    `broadcast_address: true` is skipped: a broadcast/global-call address
    is legitimately shared by design (e.g. TAS2563's 0x48, see
    metadata/chips/tas2563.yaml) -- do not reach for that opt-out to
    silence a real strap conflict.

    Returns a failure list shaped like `_check_files()`.
    """
    failures: list[tuple[Path, list[str]]] = []
    for path in board_files:
        rel = path.relative_to(REPO).as_posix()
        try:
            doc = strict_yaml_load(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            continue  # parse errors already reported by the schema pass
        if not isinstance(doc, dict):
            continue

        msgs: list[str] = []
        checked = 0

        # i2c_devices[] -- one implicit on-board bus per file.
        seen: dict[int, list[dict]] = {}
        for dev in _dict_entries(doc.get("i2c_devices")):
            if dev.get("broadcast_address") is True:
                continue
            addr = dev.get("address")
            if not isinstance(addr, str) or not re.fullmatch(r"0x[0-9A-Fa-f]{2}", addr):
                continue
            checked += 1
            seen.setdefault(int(addr, 16), []).append(dev)
        for addr_int, devs in sorted(seen.items()):
            if len(devs) < 2:
                continue
            names = ", ".join(f"{d.get('part')}/{d.get('designator')}" for d in devs)
            msgs.append(
                f"i2c_devices: {names} all declare address=0x{addr_int:02X} "
                f"on the board's on-board I2C bus -- two devices cannot "
                f"share a fixed I2C address (#1845); set "
                f"broadcast_address: true only if this is a real "
                f"broadcast/global-call address")

        # audio.codecs[] -- each entry names its own bus.
        seen_by_bus: dict[tuple[str, int], list[dict]] = {}
        for dev in _dict_entries(_as_dict(doc.get("audio")).get("codecs")):
            if dev.get("broadcast_address") is True:
                continue
            bus = dev.get("i2c_bus")
            addr = dev.get("i2c_address")
            if not isinstance(bus, str) or not isinstance(addr, str):
                continue
            if not re.fullmatch(r"0x[0-9A-Fa-f]{1,2}", addr):
                continue
            checked += 1
            seen_by_bus.setdefault((bus, int(addr, 16)), []).append(dev)
        for (bus, addr_int), devs in sorted(seen_by_bus.items()):
            if len(devs) < 2:
                continue
            names = ", ".join(f"{d.get('chip')}/{d.get('designator')}" for d in devs)
            msgs.append(
                f"audio.codecs: {names} all declare i2c_address=0x{addr_int:02X} "
                f"on {bus} -- two devices cannot share a fixed I2C address "
                f"(#1845); set broadcast_address: true only if this is a "
                f"real broadcast/global-call address")

        if msgs:
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
            failures.append((rel, msgs))
        else:
            print(f"OK   {rel}  (i2c address(es): {checked} checked, no collisions)")
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


def _check_supervisor_links_cross_refs(supervisor_links_files) -> list:
    """Cross-check metadata/e1m_modules/v2n/supervisor-links.yaml against
    the pad-ownership ground truth in metadata/pinmux/v2n.yaml, and the
    GD32 I2C address against metadata/chips/gd32g553.yaml (#655).

    JSON Schema validates each link's shape but has no way to express a
    cross-file reference: every (silicon_peripheral, silicon_pad) pair
    this file claims for the GD32 supervisor bridge -- every pin row plus
    the `gd32_spi.gpio_chip_select` entry -- must resolve to EXACTLY one
    `owner: "renesas"` row in metadata/pinmux/v2n.yaml.  Zero matches or
    more than one is a hard error naming the offending pair.  Where that
    matched row itself carries a `core:` key, the value MUST be "m33" --
    but a matched row with NO `core:` key is not an error: the console's
    UART0_TXD0/UART0_RXD0 rows legitimately carry no `core:` attribution
    in metadata/pinmux/v2n.yaml (see
    metadata/e1m_modules/v2n/core-ownership.yaml's own note on why
    absence is never treated as "a55 by elimination"), so requiring
    `core: "m33"` unconditionally would fail the console link.

    Also cross-checks `brd_i2c.peer_address_7bit` against
    metadata/chips/gd32g553.yaml `i2c.default_address_7bit` -- the value
    is recorded in supervisor-links.yaml to be CROSS-CHECKED, not as an
    independent authority.

    Also cross-checks every pin row's `pfc_port`/`pfc_pin` against its own
    `silicon_pad`: the two restate the same fact (`silicon_pad: "P76"`
    implies `pfc_port: "PORT_07"`, `pfc_pin: 6`) and nothing else in this
    file catches a typo'd pairing -- a mismatched `pfc_port`/`pfc_pin`
    would emit a wrong `RZV_PINMUX(...)` to real silicon even though the
    `silicon_pad` alone still resolves cleanly against
    metadata/pinmux/v2n.yaml above. Only pads of the `P<digit><digit>`
    shape are derivable this way; a pad that doesn't match is a hard
    error too (naming the pad), never a silent skip, so a future non-`Pnn`
    pad shape forces a deliberate decision here instead of quietly losing
    the check.

    Returns a failure list shaped like `_check_files()`.
    """
    failures: list[tuple[Path, list[str]]] = []
    if not supervisor_links_files:
        return failures
    path = supervisor_links_files[0]
    rel = path.relative_to(REPO).as_posix()
    try:
        doc = strict_yaml_load(path.read_text(encoding="utf-8"), source=path)
    except Exception:
        return failures  # parse errors already reported by the schema pass
    if not isinstance(doc, dict):
        return failures
    links = _as_dict(doc.get("supervisor_links"))
    if not links:
        return failures

    msgs: list[str] = []

    pinmux_path = REPO / "metadata" / "pinmux" / "v2n.yaml"
    pads_by_pair: dict[tuple[str, str], list[dict]] = {}
    if not pinmux_path.is_file():
        msgs.append(
            f"no {pinmux_path.relative_to(REPO).as_posix()} -- cannot "
            f"cross-check pad ownership")
    else:
        try:
            pm_doc = strict_yaml_load(
                pinmux_path.read_text(encoding="utf-8"), source=pinmux_path)
        except Exception as e:
            msgs.append(
                f"cannot cross-check against "
                f"{pinmux_path.relative_to(REPO).as_posix()}: parse error ({e})")
            pm_doc = None
        if isinstance(pm_doc, dict):
            for row in _dict_entries(pm_doc.get("pads")):
                sp, pad = row.get("silicon_peripheral"), row.get("silicon_pad")
                if isinstance(sp, str) and isinstance(pad, str):
                    pads_by_pair.setdefault((sp, pad), []).append(row)

    def _check_pair(sp: object, pad: object, where: str) -> None:
        if not isinstance(sp, str) or not isinstance(pad, str):
            return  # already a schema-shape violation reported elsewhere
        matches = [r for r in pads_by_pair.get((sp, pad), [])
                   if r.get("owner") == "renesas"]
        if len(matches) != 1:
            msgs.append(
                f"{where}: (silicon_peripheral={sp!r}, silicon_pad={pad!r}) "
                f"matches {len(matches)} owner=\"renesas\" row(s) in "
                f"metadata/pinmux/v2n.yaml (need exactly 1)")
            return
        core = matches[0].get("core")
        if core is not None and core != "m33":
            msgs.append(
                f"{where}: (silicon_peripheral={sp!r}, silicon_pad={pad!r}) "
                f"resolves to a metadata/pinmux/v2n.yaml row with "
                f"core={core!r}, expected \"m33\"")

    _PAD_SHAPE = re.compile(r"^P([0-9])([0-9])$")

    def _check_pad_derivation(pad: object, pfc_port: object, pfc_pin: object,
                               where: str) -> None:
        if not isinstance(pad, str):
            return  # already a schema-shape violation reported elsewhere
        m = _PAD_SHAPE.match(pad)
        if not m:
            msgs.append(
                f"{where}: silicon_pad={pad!r} does not match the "
                f"P<port-digit><pin-digit> shape this derivation check "
                f"understands -- add explicit handling for this pad shape "
                f"rather than silently skipping the pfc_port/pfc_pin check")
            return
        expected_port = f"PORT_0{m.group(1)}"
        expected_pin = int(m.group(2))
        if pfc_port != expected_port or pfc_pin != expected_pin:
            msgs.append(
                f"{where}: silicon_pad={pad!r} implies "
                f"pfc_port={expected_port!r}, pfc_pin={expected_pin} but "
                f"this row declares pfc_port={pfc_port!r}, "
                f"pfc_pin={pfc_pin!r}")

    for link_name, link in sorted(links.items()):
        if not isinstance(link, dict):
            continue
        for pin in _dict_entries(link.get("pins")):
            sp, pad = pin.get("silicon_peripheral"), pin.get("silicon_pad")
            _check_pair(sp, pad, f"supervisor_links.{link_name}.pins")
            _check_pad_derivation(pad, pin.get("pfc_port"), pin.get("pfc_pin"),
                                   f"supervisor_links.{link_name}.pins")
        gcs = link.get("gpio_chip_select")
        if isinstance(gcs, dict):
            _check_pair(gcs.get("silicon_peripheral"), gcs.get("silicon_pad"),
                        f"supervisor_links.{link_name}.gpio_chip_select")

    brd_i2c = links.get("brd_i2c")
    if isinstance(brd_i2c, dict) and "peer_address_7bit" in brd_i2c:
        chip_path = REPO / "metadata" / "chips" / "gd32g553.yaml"
        if not chip_path.is_file():
            msgs.append(
                f"no {chip_path.relative_to(REPO).as_posix()} -- cannot "
                f"cross-check brd_i2c.peer_address_7bit")
        else:
            try:
                chip_doc = strict_yaml_load(
                    chip_path.read_text(encoding="utf-8"), source=chip_path)
            except Exception as e:
                msgs.append(
                    f"cannot cross-check brd_i2c.peer_address_7bit against "
                    f"{chip_path.relative_to(REPO).as_posix()}: parse error ({e})")
                chip_doc = None
            if isinstance(chip_doc, dict):
                chip_addr = _as_dict(chip_doc.get("i2c")).get("default_address_7bit")
                link_addr = brd_i2c.get("peer_address_7bit")
                if chip_addr != link_addr:
                    msgs.append(
                        f"supervisor_links.brd_i2c.peer_address_7bit="
                        f"{link_addr!r} does not match "
                        f"{chip_path.relative_to(REPO).as_posix()} "
                        f"i2c.default_address_7bit={chip_addr!r}")

    if msgs:
        print(f"FAIL {rel}")
        for m in msgs:
            print(f"  · {m}")
        failures.append((rel, msgs))
    else:
        print(f"OK   {rel}  (supervisor_links cross-checked against "
              f"metadata/pinmux/v2n.yaml + metadata/chips/gd32g553.yaml)")
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


_MODEL_PERF_FIXTURE_MARKER = "_fixture"
_MODEL_PERF_LATENCY_RUN_FLOOR = 30
_MODEL_PERF_ALLOWED_ROOT_FILES = {"README.md"}


def _collect_model_perf_files(root: Path) -> tuple[list[Path], list[tuple[str, list[str]]]]:
    """Collect metadata/model_perf/<SKU>/<hash>.yaml -- and FAIL loudly on
    anything that doesn't fit that exact two-level shape, instead of
    silently skipping it (issue #1520 review, PR #1884).

    The one-level `MODEL_PERF.glob("*/*.yaml")` this replaces never opens a
    point placed one directory too deep (`<SKU>/_fixture/<hash>.yaml` --
    the precise evasion the `_MODEL_PERF_FIXTURE_MARKER` refusal below
    exists to catch), a `.yml` sibling, or a stray file dropped directly
    under `root` -- none of those reach the schema or semantic pass, so
    the gate prints a clean `0 failure(s)` for a file nobody looked at.

    Walks the whole tree with `rglob("*")` and classifies every FILE it
    finds (directories are structure, not data):
      * `root/README.md` is the tree's own doc -- the one root-level file
        allowed, silently skipped;
      * `root/<dir>/<name>.yaml` (exactly two path segments below `root`,
        `.yaml` -- not `.yml` -- suffix) is a real candidate, returned for
        the schema + semantic passes to validate;
      * anything else -- a stray root-level file, a nested-one-level-too-
        deep file, a non-`.yaml` sibling -- is a structural violation and
        comes back as a FAILURE, shaped like every other check's failure
        list, not a silent skip.
    """
    candidates: list[Path] = []
    failures: list[tuple[str, list[str]]] = []
    if not root.is_dir():
        return candidates, failures
    for path in sorted(root.rglob("*")):
        if path.is_dir():
            continue
        try:
            rel = path.relative_to(REPO).as_posix()
        except ValueError:
            rel = path.as_posix()
        parts = path.relative_to(root).parts
        if len(parts) == 1 and path.name in _MODEL_PERF_ALLOWED_ROOT_FILES:
            continue  # the tree's own doc, not a data file
        if len(parts) != 2:
            failures.append((rel, [
                f"sits {len(parts)} path segment(s) below metadata/model_perf/ "
                f"-- a real point is exactly <SKU>/<hash>.yaml (2 segments); "
                f"a file this shallow or this deep is never opened by the "
                f"schema/semantic passes and validates nothing"]))
            continue
        if path.suffix != ".yaml":
            failures.append((rel, [
                f"extension `{path.suffix}` is not `.yaml` -- collection "
                f"only looks for *.yaml, so this file is silently invisible "
                f"to every check below"]))
            continue
        candidates.append(path)
    return candidates, failures


def _model_perf_identity_hash(doc) -> str:
    """16-hex-char content hash of a model-perf point's MEASUREMENT identity
    (issue #1520) -- the single source `docs/bench/model-perf-capture.md` and
    `metadata/schemas/model-perf-v1.schema.json` both point back at.

    Deliberately keyed on the full measurement context (SoM SKU + hw_rev +
    compile target, including the exact compiler build + the exact
    source-model bytes + the vela profile when one applies) rather than on
    the model alone or the SoM alone: two points that share a model but
    differ in backend/accel_config/core/compiler_version/vela profile are
    different measurements and must not collide on one path.  Changing ANY
    identity field must produce a different hash, so a stale filename can
    never silently point at an edited body -- `_check_model_perf_semantics()`
    below is what enforces that the on-disk filename actually matches this.

    `target.compiler_version` (e.g. `vela 4.1.0`) is in this key because a
    compiler upgrade alone -- no other identity field changing -- can move
    `arena_bytes`/`latency_ms`: a point captured under vela 4.1.0 and the
    same point re-captured under vela 5.x would otherwise hash identically
    and the second capture would silently overwrite the first at the same
    filename (issue #1520 review, PR #1884).  Two fields the same review
    raised are DELIBERATELY left out of this key for now: vela's
    `--optimise`/`--arena-cache-size` flags and the core/NPU clock. Neither
    has a machine-source field anywhere in this repo today (unlike
    compiler_version, which already lives on the `.alpmodel` manifest's
    `Target.compiler_version` and `scripts/alp_model/adapters/ethos_u.py`'s
    `_vela_version()`) -- adding them here would mean inventing new capture
    plumbing this contract doesn't build, and an identity field nothing
    writes is worse than no field: it can never be verified, only trusted.
    Revisit once a capture path actually records them.
    """
    target = _as_dict(doc.get("target")) if isinstance(doc, dict) else {}
    model = _as_dict(doc.get("model")) if isinstance(doc, dict) else {}
    vela = doc.get("vela") if isinstance(doc, dict) else None
    vela = vela if isinstance(vela, dict) else {}
    parts = [
        str(doc.get("sku", "")) if isinstance(doc, dict) else "",
        str(doc.get("hw_rev", "")) if isinstance(doc, dict) else "",
        str(target.get("backend", "")),
        str(target.get("accel_config", "")),
        str(target.get("core", "")),
        str(target.get("compiler_version", "")),
        str(model.get("src_sha", "")),
        str(vela.get("system_config", "")),
        str(vela.get("memory_mode", "")),
    ]
    return hashlib.sha256("|".join(parts).encode("utf-8")).hexdigest()[:16]


def _model_perf_target_context(sku: str):
    """(target_pairs, core_ids, paired_core_by_target) for a SKU, or None
    when the SKU itself can't be resolved (the caller already reports that
    separately as "SKU exists").

    target_pairs -- the (backend, accel_config) pairs
    `alp_model.targets.resolve_targets()` actually resolves for this SKU --
    the SAME resolver `alp model check` uses, so a perf point can't name a
    target the tiered resolution could never route a compile to.

    core_ids -- the SoM preset's `topology:` role keys (e.g. `m55_hp`,
    `a32_cluster`) -- the core-name vocabulary for this SKU.

    paired_core_by_target -- for the subset of the HOST SoC's `npus[]`
    entries that pin a `paired_core`, the one core id that (backend,
    accel_config) target is allowed to name.  An NPU entry with no
    `paired_core` (accessible from more than one core, or not yet known)
    imposes no stricter constraint than "any topology core id" here --
    `accel_config`'s one-line format mirrors `targets.py::_soc_targets()`,
    the only other place this string is built, deliberately kept in sync by
    hand rather than by extending that function's return shape for one
    caller.
    """
    preset_path = SOM_PRESETS / f"{sku}.yaml"
    try:
        specs = resolve_targets(sku, metadata_root=SOM_PRESETS.parent)
        preset = strict_yaml_load(preset_path.read_text(encoding="utf-8"), source=preset_path)
    except Exception:
        return None
    if not isinstance(preset, dict):
        return None

    target_pairs = {(s.backend, s.accel_config) for s in specs}
    topology = preset.get("topology")
    core_ids = set(topology.keys()) if isinstance(topology, dict) else set()

    paired: dict[tuple[str, str], str] = {}
    silicon = str(preset.get("silicon", ""))
    soc_path = resolve_soc_path(silicon, SOM_PRESETS.parent)
    if soc_path is not None and soc_path.is_file():
        try:
            soc = json.loads(soc_path.read_text(encoding="utf-8"))
        except Exception:
            soc = {}
        for npu in _dict_entries(soc.get("npus") if isinstance(soc, dict) else None):
            pc = npu.get("paired_core")
            if not isinstance(pc, str) or not pc:
                continue
            backend = _npu_backend(str(npu.get("type", "")), str(npu.get("subtype", "")))
            if backend is None:
                continue
            accel = _accel_config(npu, backend)
            paired[(backend, accel)] = pc

    return target_pairs, core_ids, paired


def _check_model_perf_semantics(model_perf_files) -> list:
    """metadata/model_perf/<SKU>/<hash>.yaml semantic cross-checks a JSON
    Schema shape pass can't express (issue #1520): the path reproduces the
    body; the SKU exists; the (backend, accel_config) pair and the core are
    ones the SKU actually resolves; hw_rev is in the family table; an
    ethos_u point records its vela profile and a non-ethos_u point carries
    none; req_sram_kib covers arena_bytes; p95 is not below the mean and
    p50 is not above p95; the run-count floor; capture.date parses; the
    published tree cannot absorb a `_fixture`.  Returns a failure list
    shaped like `_check_files()`.
    """
    failures: list[tuple[str, list[str]]] = []  # (rel-path str, msgs), not Path -- see below
    for path in model_perf_files:
        try:
            rel = path.relative_to(REPO).as_posix()
        except ValueError:
            rel = path.as_posix()
        msgs: list[str] = []

        # The published tree cannot absorb a `_fixture` -- checked on the
        # PATH alone, before the body is even parsed: a test/dev fixture
        # checked in here is wrong regardless of whether its body validates.
        # Scoped to the two path components that are actually PART of the
        # published-tree naming -- the SKU directory and the identity-hash
        # filename -- rather than the full absolute path: an ancestor
        # directory (a developer's checkout path, a CI workspace) can
        # coincidentally contain this substring with nothing to do with the
        # tree's own content, which a whole-path scan would misreport.
        if (_MODEL_PERF_FIXTURE_MARKER in path.parent.name
                or _MODEL_PERF_FIXTURE_MARKER in path.name):
            msgs.append(
                f"path contains `{_MODEL_PERF_FIXTURE_MARKER}` -- a fixture "
                f"belongs under tests/fixtures/model_perf/, never in the "
                f"published metadata/model_perf/ tree")

        try:
            doc = strict_yaml_load(path.read_text(encoding="utf-8"), source=path)
        except Exception:
            doc = None  # parse errors already reported by the schema pass
        if not isinstance(doc, dict):
            if msgs:
                print(f"FAIL {rel}")
                for m in msgs:
                    print(f"  · {m}")
                failures.append((rel, msgs))
            continue

        sku = doc.get("sku")
        hw_rev = doc.get("hw_rev")
        target = _as_dict(doc.get("target"))
        model = _as_dict(doc.get("model"))
        vela = doc.get("vela")
        perf = _as_dict(doc.get("perf"))
        capture = _as_dict(doc.get("capture"))
        backend = target.get("backend")
        accel_config = target.get("accel_config", "")
        core = target.get("core")

        # The path reproduces the body: the containing directory IS the SKU,
        # and the filename IS the measurement-identity hash of this body.
        if isinstance(sku, str) and sku:
            if path.parent.name != sku:
                msgs.append(
                    f"path directory `{path.parent.name}` != body `sku: "
                    f"{sku}` -- the containing directory is the SKU, not an "
                    f"independently-chosen label")
            expected_stem = _model_perf_identity_hash(doc)
            if path.stem != expected_stem:
                msgs.append(
                    f"filename `{path.stem}` doesn't reproduce this body's "
                    f"measurement-identity hash (`{expected_stem}`) -- "
                    f"sku/hw_rev/target/model.src_sha/vela changed without "
                    f"renaming the file, or two different measurements "
                    f"collided on one path")

        # The SKU exists.
        som_ctx = None
        if not isinstance(sku, str) or not sku:
            msgs.append("sku: missing/not a string")
        elif not (SOM_PRESETS / f"{sku}.yaml").is_file():
            msgs.append(f"sku `{sku}`: no metadata/e1m_modules/{sku}.yaml preset")
        else:
            som_ctx = _model_perf_target_context(sku)

        # The (backend, accel_config) pair and the core are ones the SKU
        # actually resolves.
        if som_ctx is not None:
            target_pairs, core_ids, paired = som_ctx
            key = (backend, accel_config)
            if key not in target_pairs:
                msgs.append(
                    f"target: (backend={backend!r}, accel_config="
                    f"{accel_config!r}) is not a target `{sku}` actually "
                    f"resolves (alp_model.targets.resolve_targets) -- valid: "
                    f"{sorted(target_pairs)}")
            if not isinstance(core, str) or not core:
                msgs.append("target.core: missing/not a string")
            elif core_ids and core not in core_ids:
                msgs.append(
                    f"target.core `{core}` is not a `topology:` role of "
                    f"`{sku}` -- valid: {sorted(core_ids)}")
            required_core = paired.get(key)
            if required_core is not None and core != required_core:
                msgs.append(
                    f"target.core `{core}` != `{required_core}`, the core "
                    f"this SoC JSON pins (backend={backend!r}, accel_config="
                    f"{accel_config!r}) to via npus[].paired_core")

        # hw_rev is in the family table.
        if not isinstance(hw_rev, str) or not hw_rev:
            msgs.append("hw_rev: missing/not a string")
        elif isinstance(sku, str) and sku:
            try:
                family = _sku_family(sku)
            except ValueError:
                family = None
            if family is not None:
                hwrev_path = SOM_PRESETS / family / "hw-revisions.yaml"
                if not hwrev_path.is_file():
                    msgs.append(
                        f"hw_rev `{hw_rev}`: no metadata/e1m_modules/"
                        f"{family}/hw-revisions.yaml family table to check "
                        f"against")
                else:
                    try:
                        table = strict_yaml_load(
                            hwrev_path.read_text(encoding="utf-8"), source=hwrev_path)
                    except Exception:
                        table = None
                    revs = _as_dict(table.get("hw_revisions")) if isinstance(table, dict) else {}
                    if hw_rev not in revs:
                        msgs.append(
                            f"hw_rev `{hw_rev}` is not a key in "
                            f"metadata/e1m_modules/{family}/hw-revisions.yaml "
                            f"hw_revisions: -- valid: {sorted(revs)}")

        # An ethos_u point records its vela profile.
        if backend == "ethos_u":
            if not isinstance(vela, dict):
                msgs.append(
                    "target.backend is `ethos_u` but `vela:` is missing -- "
                    "vela silently falls back to its OWN built-in default "
                    "(Ethos_U85_SYS_DRAM_Mid / Dedicated_Sram_384KB, a "
                    "DRAM-backed profile) when --system-config/--memory-mode "
                    "aren't passed; record whichever profile this capture "
                    "actually used")
            else:
                if not vela.get("system_config"):
                    msgs.append("vela.system_config: missing/empty")
                if not vela.get("memory_mode"):
                    msgs.append("vela.memory_mode: missing/empty")
        elif isinstance(vela, dict):
            # A `vela:` block on a non-ethos_u point is meaningless (no vela
            # compile happened) and hashes into the identity for nothing --
            # a stray copy-paste from an ethos_u point silently produces a
            # different hash for what is otherwise the same measurement
            # (issue #1520 review, PR #1884).
            msgs.append(
                f"target.backend is `{backend}`, not `ethos_u`, but `vela:` "
                f"is present -- vela only runs for an ethos_u target; drop "
                f"this block (it plays no part in a {backend} compile)")

        # req_sram_kib covers arena_bytes.
        req_sram_kib = perf.get("req_sram_kib")
        arena_bytes = perf.get("arena_bytes")
        if isinstance(req_sram_kib, int) and isinstance(arena_bytes, int):
            if req_sram_kib * 1024 < arena_bytes:
                msgs.append(
                    f"perf.req_sram_kib ({req_sram_kib} KiB = "
                    f"{req_sram_kib * 1024} B) is smaller than "
                    f"perf.arena_bytes ({arena_bytes} B) -- the declared "
                    f"SRAM budget doesn't cover the compiler-reported arena "
                    f"it's supposed to hold")

        # p95 is not below the mean; p50 is not above p95; the run-count floor.
        latency = perf.get("latency_ms")
        if isinstance(latency, dict):
            def _num(v):
                return isinstance(v, (int, float)) and not isinstance(v, bool)
            mean, p50, p95, runs = (latency.get("mean"), latency.get("p50"),
                                     latency.get("p95"), latency.get("runs"))
            if _num(mean) and _num(p95) and p95 < mean:
                msgs.append(
                    f"perf.latency_ms.p95 ({p95}) is below "
                    f"perf.latency_ms.mean ({mean}) -- a p95 below the mean "
                    f"of the same sample is not a valid percentile (mean/"
                    f"p50/p95 swapped, or a stale value left over from a "
                    f"re-run)")
            if _num(p50) and _num(p95) and p95 < p50:
                # p50 <= p95 always holds for any real sample (a percentile
                # function is non-decreasing) -- unlike mean-vs-p50, which a
                # right-skewed latency tail can legitimately invert, so that
                # relationship is deliberately NOT enforced here (issue
                # #1520 review, PR #1884).
                msgs.append(
                    f"perf.latency_ms.p50 ({p50}) is above "
                    f"perf.latency_ms.p95 ({p95}) -- the 50th percentile of "
                    f"a sample can never exceed its 95th percentile (mean/"
                    f"p50/p95 swapped, or a stale value left over from a "
                    f"re-run)")
            if isinstance(runs, int) and not isinstance(runs, bool) and runs < _MODEL_PERF_LATENCY_RUN_FLOOR:
                msgs.append(
                    f"perf.latency_ms.runs ({runs}) is below the floor of "
                    f"{_MODEL_PERF_LATENCY_RUN_FLOOR} -- a p95 over fewer "
                    f"runs is noise, not a percentile")

        # capture.date parses.
        date = capture.get("date")
        if not isinstance(date, str):
            msgs.append("capture.date: missing/not a string")
        else:
            try:
                datetime.date.fromisoformat(date)
            except ValueError:
                msgs.append(
                    f"capture.date `{date}` does not parse as an ISO-8601 "
                    f"date (YYYY-MM-DD)")

        if msgs:
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
            failures.append((rel, msgs))
        else:
            print(f"OK   {rel}  (sku={sku}, target={backend}/"
                  f"{accel_config or 'cpu'}/{core})")
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
            # #1845: two chips declaring the same (bus, address) reaches
            # silicon as two devices answering one address.
            board_failures += _check_board_i2c_address_collisions(board_files)

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

    # V2N/V2M on-module GD32G553 supervisor pin-wiring source (#655)
    # against supervisor-links-v1.
    supervisor_links_failures: list = []
    supervisor_links_files: list = []
    if SUPERVISOR_LINKS_SCHEMA.is_file():
        # SUPERVISOR_LINKS_DATA is an explicit single-file constant, not a
        # glob -- a deleted/renamed data file used to silently no-op this
        # whole registration (`0 supervisor-links file(s) checked, 0
        # failure(s)`, exit 0). That is a hard error, not a skip: the
        # V2N/V2M pinctrl.dtsi/_defconfig emitters have no other source.
        if not SUPERVISOR_LINKS_DATA.is_file():
            rel = SUPERVISOR_LINKS_DATA.relative_to(REPO).as_posix()
            msg = ("missing -- the V2N/V2M on-module GD32G553 supervisor "
                   "pin-wiring source (#655) must exist at this exact path")
            print()
            print(f"FAIL {rel}")
            print(f"  · {msg}")
            supervisor_links_failures.append((rel, [msg]))
        else:
            sl_schema = json.loads(SUPERVISOR_LINKS_SCHEMA.read_text(encoding="utf-8"))
            sl_validator = jsonschema.Draft202012Validator(sl_schema)
            supervisor_links_files = [SUPERVISOR_LINKS_DATA]
            print()
            supervisor_links_failures = _check_files(
                "YAML", supervisor_links_files, sl_validator,
                lambda p: strict_yaml_load(p.read_text(encoding="utf-8"), source=p),
                "schemaVersion",
            )
            # Cross-ref against metadata/pinmux/v2n.yaml (pad ownership +
            # core attribution) and metadata/chips/gd32g553.yaml (peer I2C
            # address) -- neither is expressible in the schema alone.
            supervisor_links_failures += _check_supervisor_links_cross_refs(supervisor_links_files)

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

    # Per-NPU op-support lists (the static-analyzer data asset).
    npu_ops_failures: list = []
    npu_ops_files: list = []
    if NPU_OPS_SCHEMA.is_file():
        npu_ops_schema = json.loads(NPU_OPS_SCHEMA.read_text(encoding="utf-8"))
        npu_ops_validator = jsonschema.Draft202012Validator(npu_ops_schema)
        npu_ops_files = sorted(NPU_OPS.glob("*.json"))
        if npu_ops_files:
            print()
            npu_ops_failures = _check_files(
                "JSON", npu_ops_files, npu_ops_validator,
                lambda p: json.loads(p.read_text(encoding="utf-8")),
                "backend",
            )

    # Model-zoo entries (YAML) against model-zoo v1.
    model_zoo_failures: list = []
    model_zoo_files: list = []
    if MODEL_ZOO_SCHEMA.is_file():
        model_zoo_schema = json.loads(MODEL_ZOO_SCHEMA.read_text(encoding="utf-8"))
        model_zoo_validator = jsonschema.Draft202012Validator(model_zoo_schema)
        model_zoo_files = sorted(MODEL_ZOO.glob("*.yaml"))
        if model_zoo_files:
            print()
            model_zoo_failures = _check_files(
                "YAML", model_zoo_files, model_zoo_validator,
                lambda p: yaml.safe_load(p.read_text(encoding="utf-8")),
                "id",
            )

    # Tier-2 model-perf points (YAML) against model-perf v1 (#1520).
    # metadata/model_perf/ ships EMPTY today -- a perf point comes off real
    # silicon or it does not exist (docs/bench/model-perf-capture.md) -- so
    # this section exists to gate the FIRST bench capture from day one,
    # rather than being bolted on after the tree already has content in it.
    model_perf_failures: list = []
    model_perf_files: list = []
    if MODEL_PERF_SCHEMA.is_file():
        model_perf_schema = json.loads(MODEL_PERF_SCHEMA.read_text(encoding="utf-8"))
        model_perf_validator = jsonschema.Draft202012Validator(model_perf_schema)
        model_perf_files, model_perf_collector_failures = _collect_model_perf_files(MODEL_PERF)
        if model_perf_files or model_perf_collector_failures:
            print()
        for rel, msgs in model_perf_collector_failures:
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
        model_perf_failures += model_perf_collector_failures
        if model_perf_files:
            model_perf_failures += _check_files(
                "YAML", model_perf_files, model_perf_validator,
                lambda p: strict_yaml_load(p.read_text(encoding="utf-8"), source=p),
                "sku",
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

    # SoM `on_module.i2c_devices.<bus>.devices[]` (bus, address_7bit) uniqueness (#1845).
    i2c_collision_failures: list = []
    if som_files:
        print()
        i2c_collision_failures = _check_som_i2c_address_collisions(som_files)

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
                      + len(model_zoo_failures)
                      + len(model_perf_failures)
                      + len(library_failures) + len(library_semantic_failures)
                      + len(board_target_failures)
                      + len(restriction_failures)
                      + len(instance_uniqueness_failures)
                      + len(slot0_address_failures)
                      + len(i2c_collision_failures)
                      + len(silicon_kconfig_failures)
                      + len(peripheral_kconfig_failures)
                      + len(tier_a_library_ci_failures)
                      + len(supervisor_links_failures))
    print(f"{len(soc_files)} SoC file(s) + {len(som_files)} SoM preset(s) + "
          f"{len(hwrev_files)} hw-revisions file(s) + "
          f"{len(board_files)} board preset(s) + {len(chip_files)} chip file(s) + "
          f"{len(block_files)} block file(s) + "
          f"{len(npu_ops_files)} npu-ops file(s) + "
          f"{len(model_zoo_files)} model-zoo entry(ies) + "
          f"{len(model_perf_files)} model-perf point(s) + "
          f"{len(library_files)} library manifest(s) + Kconfig registries + "
          f"tier-a-library-ci registry + "
          f"{len(supervisor_links_files)} supervisor-links file(s) "
          f"checked, {total_failures} failure(s)")
    return 0 if total_failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
