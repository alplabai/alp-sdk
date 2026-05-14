#!/usr/bin/env python3
# Copyright 2026 ALP Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Cross-family portability lint for examples/*/board.yaml.

What this catches
-----------------

The ALP SDK's portability story is layered:

  Ring 1 -- cross-family examples.  Same main.c builds on every E1M-X
            family.  Declares no chip drivers in board.yaml; relies
            on <alp/peripheral.h> + the thin wrappers.

  Ring 2 -- chip-bound examples.  Uses <alp/chips/<chip>.h> but the
            chip is populated on multiple SoM families, so the
            example runs unchanged on any of them.

  Ring 3 -- SoM-bound examples.  Uses a chip that only one family
            populates.  Customer can copy the example but it won't
            build cleanly on a different family.

The lint enforces two invariants:

  (a) HARD ERROR: an example's `chips:` list MUST NOT contain a
      chip whose `families:` field in metadata/chips/<chip>.yaml
      excludes the example's own som.sku family.  This catches the
      pathological case where someone wires `deepx_dxm1` into an
      AEN board.yaml; the build would fail downstream but the
      diagnostic would be cryptic.

  (b) INFO: classify each example into its ring and print the
      breakdown so a customer reading CI output (or the maintainer)
      can confirm the portability claim in examples/README.md
      matches reality.

Run from the alp-sdk repo root:

    python3 scripts/check_example_portability.py

Exits non-zero on a hard error.
"""

from __future__ import annotations

import argparse
import pathlib
import sys
from typing import Optional

try:
    import yaml
except ImportError:  # pragma: no cover -- CI always has pyyaml
    print("error: pyyaml not installed (pip install pyyaml)", file=sys.stderr)
    sys.exit(1)


ROOT = pathlib.Path(__file__).resolve().parent.parent

# Map som.sku prefix -> family name as stored in chip metadata.
# E1M-AEN... -> aen
# E1M-V2N... -> v2n
# E1M-V2M... -> v2n-m1   (E1M-V2M is the V2N-M1 part numbering)
# E1M-NX9... -> imx93
_SKU_FAMILY_TABLE = (
    ("E1M-V2M", "v2n-m1"),
    ("E1M-V2N", "v2n"),
    ("E1M-AEN", "aen"),
    ("E1M-NX9", "imx93"),
)


def som_family_for_sku(sku: str) -> Optional[str]:
    """Return the SoM family slug for an E1M SKU, or None if unrecognised."""
    for prefix, family in _SKU_FAMILY_TABLE:
        if sku.startswith(prefix):
            return family
    return None


def load_chip_families() -> dict[str, list[str]]:
    """Map chip_id -> families list, scraped from metadata/chips/*.yaml."""
    out: dict[str, list[str]] = {}
    for chip_yaml in sorted((ROOT / "metadata" / "chips").glob("*.yaml")):
        with chip_yaml.open(encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
        chip_id = doc.get("chip_id")
        families = doc.get("families", [])
        if isinstance(chip_id, str) and isinstance(families, list):
            out[chip_id] = [str(f) for f in families]
    return out


def load_som_optional_chips() -> dict[str, set[str]]:
    """Map som.sku -> set of chip_ids whose `assembled` flag isn't a hard
    true.  Used to surface "chip is BOM-optional on this SKU" warnings."""
    out: dict[str, set[str]] = {}
    for som_yaml in sorted((ROOT / "metadata" / "e1m_modules").glob("E1M-*.yaml")):
        with som_yaml.open(encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
        sku = doc.get("sku")
        if not isinstance(sku, str):
            continue
        optional: set[str] = set()
        i2c_buses = ((doc.get("on_module") or {}).get("i2c_devices") or {})
        for bus_doc in i2c_buses.values():
            if not isinstance(bus_doc, dict):
                continue
            for dev in bus_doc.get("devices", []) or []:
                if not isinstance(dev, dict):
                    continue
                assembled = dev.get("assembled", True)
                chip = dev.get("chip")
                if not isinstance(chip, str):
                    continue
                # BOM-variant population: anything that's not a hard True
                # means the chip might not be present on every unit of
                # this SKU.  False = DNI; "optional" = per BOM variant.
                if assembled is not True:
                    optional.add(chip)
        out[sku] = optional
    return out


def classify(chip_families: dict[str, list[str]],
             example_chips: list[str]) -> str:
    """Classify an example into ring1 / ring2 / ring3."""
    if not example_chips:
        return "ring1-cross-family"

    # Collect the intersection of families across every chip the
    # example references.  An example runs on a family iff every
    # chip it lists supports that family.
    family_sets = [set(chip_families.get(c, [])) for c in example_chips]
    if not all(family_sets):
        # At least one chip we couldn't resolve -- treat as unknown ring.
        return "ring-unknown"
    intersection = set.intersection(*family_sets) if family_sets else set()

    if len(intersection) >= 2:
        return "ring2-chip-bound"  # runs on >= 2 families
    return "ring3-som-bound"        # runs on exactly one family


def check_example(example_dir: pathlib.Path,
                  chip_families: dict[str, list[str]],
                  som_optional: dict[str, set[str]]
                  ) -> tuple[str, list[str], list[str]]:
    """Return (classification, hard-error list, info-level note list)."""
    board_yaml = example_dir / "board.yaml"
    if not board_yaml.exists():
        return "no-board-yaml", [], []
    with board_yaml.open(encoding="utf-8") as f:
        doc = yaml.safe_load(f) or {}

    som_sku = (doc.get("som") or {}).get("sku", "")
    family  = som_family_for_sku(som_sku)
    chips   = doc.get("chips") or []

    errors: list[str] = []
    notes:  list[str] = []

    if family is None and som_sku:
        errors.append(
            f"unknown som.sku prefix '{som_sku}' -- can't classify; "
            f"add the prefix to _SKU_FAMILY_TABLE in {pathlib.Path(__file__).name}"
        )

    if family is not None:
        for chip in chips:
            families = chip_families.get(chip)
            if families is None:
                errors.append(
                    f"chips: references '{chip}' but no metadata/chips/{chip}.yaml "
                    f"declares its families[] -- add the manifest"
                )
            elif family not in families:
                errors.append(
                    f"chips: references '{chip}' (families={families}) but the "
                    f"example targets som.sku '{som_sku}' (family '{family}') -- "
                    f"this example won't build on the configured SoM"
                )

        # BOM-variant note: the chip is wired into the example but the
        # SKU marks it as `assembled: optional` (or false).  The example
        # should handle the `_init() == ALP_ERR_NOT_READY` path
        # gracefully on those BOM variants.  Not a hard error.
        for chip in chips:
            if chip in som_optional.get(som_sku, set()):
                notes.append(
                    f"chip '{chip}' is BOM-optional on {som_sku} -- main.c "
                    f"should handle alp_*_init returning ALP_ERR_NOT_READY"
                )

    return classify(chip_families, chips), errors, notes


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="suppress the per-example classification table",
    )
    args = parser.parse_args(argv)

    chip_families = load_chip_families()
    som_optional  = load_som_optional_chips()

    examples_dir = ROOT / "examples"
    # Walk one OR two levels deep: cross-family examples live directly
    # at examples/<name>/, SoM-specific examples live under
    # examples/<family>/<name>/ (e.g. examples/v2n/v2n-gd32-bridge-ping).
    examples: list = []
    for d in examples_dir.iterdir():
        if not d.is_dir():
            continue
        if (d / "board.yaml").exists():
            examples.append(d)
        else:
            for sub in d.iterdir():
                if sub.is_dir() and (sub / "board.yaml").exists():
                    examples.append(sub)
    examples.sort()

    if not examples:
        print(f"error: no examples found under {examples_dir}", file=sys.stderr)
        return 1

    hard_errors_total = 0
    classification: dict[str, list[str]] = {}

    for ex in examples:
        ring, errors, notes = check_example(ex, chip_families, som_optional)
        classification.setdefault(ring, []).append(ex.name)
        for e in errors:
            print(f"FAIL  {ex.name}: {e}", file=sys.stderr)
            hard_errors_total += 1
        for n in notes:
            print(f"NOTE  {ex.name}: {n}")

    if not args.quiet:
        print()
        print("Portability classification:")
        for ring in sorted(classification):
            print(f"  {ring} ({len(classification[ring])}):")
            for name in sorted(classification[ring]):
                print(f"    - {name}")
        print()

    if hard_errors_total:
        print(f"{hard_errors_total} hard error(s) -- failing.", file=sys.stderr)
        return 1

    print(f"OK: {len(examples)} example(s) checked, 0 portability errors.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
