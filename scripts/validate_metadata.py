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
SCHEMA = REPO / "metadata" / "schemas" / "soc-spec-v1.schema.json"
SOM_SCHEMA = REPO / "metadata" / "schemas" / "som-preset-v1.schema.json"
HWREV_SCHEMA = REPO / "metadata" / "schemas" / "hw-revisions-v1.schema.json"
SILICON_KCONFIG_SCHEMA = REPO / "metadata" / "schemas" / "silicon-kconfig-v1.schema.json"
SILICON_KCONFIG_REGISTRY = REPO / "metadata" / "registries" / "silicon-kconfig.json"
PERIPHERAL_KCONFIG_SCHEMA = REPO / "metadata" / "schemas" / "peripheral-kconfig-v1.schema.json"
PERIPHERAL_KCONFIG_REGISTRY = REPO / "metadata" / "registries" / "peripheral-kconfig.json"
BOARD_PRESET_SCHEMA = REPO / "metadata" / "schemas" / "board-preset.schema.json"
LIBRARY_SCHEMA = REPO / "metadata" / "schemas" / "library-v1.schema.json"
SOC_SPEC_SCHEMA = REPO / "metadata" / "schemas" / "soc-spec-v1.schema.json"
SOCS = REPO / "metadata" / "socs"
SOM_PRESETS = REPO / "metadata" / "e1m_modules"
BOARD_PRESETS = REPO / "metadata" / "boards"
LIBRARIES = REPO / "metadata" / "libraries"


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
    """
    if not isinstance(doc, dict):
        return
    if doc.get("pending_reference_manual_ingestion"):
        print(f"WARN  {rel}: pending_reference_manual_ingestion -> "
              f"peripheral counts default to zero, ALP_SOC_*_COUNT ceilings "
              f"may under-report")


def _check_files(label, files, validator, loader, key_for_summary):
    failures: list[tuple[Path, list[str]]] = []
    for path in files:
        rel = path.relative_to(REPO)
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
        rel = path.relative_to(REPO)
        try:
            doc = yaml.safe_load(path.read_text(encoding="utf-8"))
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
        parts = silicon.split(":")
        soc_path = None
        if len(parts) == 3:
            soc_path = SOCS / parts[0] / parts[1] / f"{parts[2]}.json"
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
    rel = SILICON_KCONFIG_REGISTRY.relative_to(REPO)
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
        parts = ref.split(":")
        if len(parts) != 3:
            msgs.append(f"knownSilicon[{ref}]: not a <vendor>:<family>:<part> ref")
            continue
        soc_path = SOCS / parts[0] / parts[1] / f"{parts[2]}.json"
        if not soc_path.is_file():
            msgs.append(f"knownSilicon[{ref}]: no SoC spec at "
                        f"{soc_path.relative_to(REPO)}")

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
    rel = PERIPHERAL_KCONFIG_REGISTRY.relative_to(REPO)
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
            rel = path.relative_to(REPO)
        except ValueError:
            rel = path  # out-of-tree (e.g. a test fixture); report as-is
        try:
            doc = yaml.safe_load(path.read_text(encoding="utf-8"))
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
        lambda p: json.loads(p.read_text(encoding="utf-8")),
        "ref",
    )

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
                lambda p: yaml.safe_load(p.read_text(encoding="utf-8")),
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
                lambda p: yaml.safe_load(p.read_text(encoding="utf-8")),
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
                lambda p: yaml.safe_load(p.read_text(encoding="utf-8")),
                "name",
            )

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
                lambda p: yaml.safe_load(p.read_text(encoding="utf-8")),
                "name",
            )
            library_semantic_failures = _check_library_semantics(library_files)

    # SoM `silicon_capabilities.unpopulated` <-> SoC capability cross-check.
    restriction_failures: list = []
    if som_files:
        print()
        restriction_failures = _check_silicon_capability_restrictions(som_files)

    # Silicon -> Kconfig registry + socs/ correspondence.
    print()
    silicon_kconfig_failures = _check_silicon_kconfig()
    peripheral_kconfig_failures = _check_peripheral_kconfig()

    print()
    total_failures = (len(soc_failures) + len(som_failures)
                      + len(hwrev_failures) + len(board_failures)
                      + len(library_failures) + len(library_semantic_failures)
                      + len(restriction_failures)
                      + len(silicon_kconfig_failures)
                      + len(peripheral_kconfig_failures))
    print(f"{len(soc_files)} SoC file(s) + {len(som_files)} SoM preset(s) + "
          f"{len(hwrev_files)} hw-revisions file(s) + "
          f"{len(board_files)} board preset(s) + {len(library_files)} "
          f"library manifest(s) + Kconfig registries "
          f"checked, {total_failures} failure(s)")
    return 0 if total_failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
