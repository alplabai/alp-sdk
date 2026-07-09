#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Cross-family portability lint for examples/*/{board.yaml,testcase.yaml}.

What this catches
-----------------

The Alp SDK's portability story is layered:

  Ring 1 -- cross-family examples.  Same main.c builds on every family
            it targets.  Declares no chip drivers in board.yaml, and
            its declared som.sku / supported_boards genuinely reach
            >= 2 SoM families; relies on <alp/peripheral.h> + the
            thin wrappers.

  Ring 2 -- chip-bound examples.  Uses <alp/chips/<chip>.h> but the
            chip is populated on multiple SoM families, so the
            example runs unchanged on any of them.

  Ring 3 -- SoM-bound examples.  Either uses a chip that only one
            family populates, or declares no chip but its som.sku /
            supported_boards resolve to exactly one family (e.g. a
            V2N-only board bring-up demo).  Customer can copy the
            example but it won't build cleanly on a different family.

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

  (c) HARD ERROR: every board listed in `supported_boards:` must
      have a matching Twister variant in `testcase.yaml`, selected
      by the corresponding `ALP_BOARD_<SLUG>` compiler define.  The
      catalog claim and CI build matrix must not drift apart.

  (d) HARD ERROR: source files must use the pinout namespace that
      matches the target SoM form factor.  E1M examples may use
      <alp/e1m_pinout.h> / ALP_E1M_*; E1M-X examples may use
      <alp/e1m_x_pinout.h> / ALP_E1M_X_*.  Cross-EVK code should
      usually use <alp/board.h> BOARD_* aliases instead.

Run from the alp-sdk repo root:

    python3 scripts/check_example_portability.py

Exits non-zero on a hard error.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from collections.abc import Iterator
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

_SKU_PINOUT_TABLE = (
    ("E1M-V2M", "e1m-x"),
    ("E1M-V2N", "e1m-x"),
    ("E1M-AEN", "e1m"),
    ("E1M-NX9", "e1m"),
)

# metadata/boards/<slug>.yaml's `hosts_som_families:` uses the
# vendor-style family names carried in the SoM preset's `family:`
# field (e.g. "renesas-rzv2n"); the chip-manifest `families:` lists
# (and _SKU_FAMILY_TABLE above) use the short slug ("v2n").  Mirror of
# the table in scripts/program_eeprom.py -- keep both in sync.
_VENDOR_FAMILY_TO_SLUG = {
    "alif-ensemble": "aen",
    "renesas-rzv2n": "v2n",
    "renesas-rzv2n-deepx": "v2n-m1",
    "nxp-imx9": "imx93",
}

_SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hh", ".hpp"}
_SOURCE_SKIP_DIRS = {"build", ".git", ".west", "__pycache__"}
_E1M_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]alp/e1m_pinout\.h[>"]')
_E1M_X_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]alp/e1m_x_pinout\.h[>"]')
_E1M_TOKEN_RE = re.compile(r"\bALP_E1M_(?!X_)[A-Z0-9_]+\b")
_E1M_X_TOKEN_RE = re.compile(r"\bALP_E1M_X_[A-Z0-9_]+\b")
_CHIP_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]alp/chips/([A-Za-z0-9_]+)\.h[>"]')


def som_family_for_sku(sku: str) -> Optional[str]:
    """Return the SoM family slug for an E1M SKU, or None if unrecognised."""
    for prefix, family in _SKU_FAMILY_TABLE:
        if sku.startswith(prefix):
            return family
    return None


def pinout_namespace_for_sku(sku: str) -> Optional[str]:
    """Return the pinout namespace expected for the SoM form factor."""
    for prefix, namespace in _SKU_PINOUT_TABLE:
        if sku.startswith(prefix):
            return namespace
    return None


def _strip_block_comments(text: str) -> str:
    """Remove C block comments while preserving line numbers."""
    return re.sub(r"/\*.*?\*/",
                  lambda m: "\n" * m.group(0).count("\n"),
                  text,
                  flags=re.DOTALL)


def _strip_line_comments_and_literals(line: str) -> str:
    line = re.sub(r"//.*", "", line)
    line = re.sub(r'"(?:\\.|[^"\\])*"', '""', line)
    line = re.sub(r"'(?:\\.|[^'\\])*'", "''", line)
    return line


def source_files(example_dir: pathlib.Path) -> list[pathlib.Path]:
    out: list[pathlib.Path] = []
    for p in example_dir.rglob("*"):
        if not p.is_file() or p.suffix not in _SOURCE_SUFFIXES:
            continue
        rel_parts = p.relative_to(example_dir).parts
        if any(part in _SOURCE_SKIP_DIRS for part in rel_parts[:-1]):
            continue
        out.append(p)
    return sorted(out)


def check_pinout_namespace(example_dir: pathlib.Path,
                           namespace: Optional[str]) -> list[str]:
    """Return hard errors for E1M/E1M-X pinout namespace mismatches."""
    if namespace not in {"e1m", "e1m-x"}:
        return []

    errors: list[str] = []
    for src in source_files(example_dir):
        text = src.read_text(encoding="utf-8", errors="replace")
        rel = src.relative_to(example_dir).as_posix()
        without_blocks = _strip_block_comments(text)

        for line_no, line in enumerate(without_blocks.splitlines(), start=1):
            line_no_comment = re.sub(r"//.*", "", line)
            if namespace == "e1m-x" and _E1M_INCLUDE_RE.search(line_no_comment):
                errors.append(
                    f"{rel}:{line_no}: includes alp/e1m_pinout.h but "
                    "the example targets an E1M-X SoM; use "
                    "alp/e1m_x_pinout.h or alp/board.h"
                )
            elif namespace == "e1m" and _E1M_X_INCLUDE_RE.search(line_no_comment):
                errors.append(
                    f"{rel}:{line_no}: includes alp/e1m_x_pinout.h but "
                    "the example targets an E1M SoM; use "
                    "alp/e1m_pinout.h or alp/board.h"
                )

            code = _strip_line_comments_and_literals(line)
            if namespace == "e1m-x":
                match = _E1M_TOKEN_RE.search(code)
                if match:
                    errors.append(
                        f"{rel}:{line_no}: uses {match.group(0)} but "
                        "the example targets an E1M-X SoM; use ALP_E1M_X_* "
                        "or a BOARD_* alias"
                    )
            else:
                match = _E1M_X_TOKEN_RE.search(code)
                if match:
                    errors.append(
                        f"{rel}:{line_no}: uses {match.group(0)} but "
                        "the example targets an E1M SoM; use ALP_E1M_* "
                        "or a BOARD_* alias"
                    )
    return errors


def check_chip_includes_declared(example_dir: pathlib.Path,
                                 declared_chips: list) -> list[str]:
    """Return hard errors for `<alp/chips/*.h>` includes missing from
    board.yaml's `chips:` list (issue #514).

    The family-compatibility check (a) and the ring classification
    both walk only the declared `chips:` array -- an example that
    `#include`s a chip driver without declaring it slips past both:
    it can wire an incompatible chip with no diagnostic, and it can
    misreport its own portability ring as if it used no chip at all.
    """
    declared = set(declared_chips)
    errors: list[str] = []
    seen: set[str] = set()
    for src in source_files(example_dir):
        text = _strip_block_comments(
            src.read_text(encoding="utf-8", errors="replace"))
        rel = src.relative_to(example_dir).as_posix()
        for line_no, line in enumerate(text.splitlines(), start=1):
            match = _CHIP_INCLUDE_RE.match(re.sub(r"//.*", "", line))
            if not match:
                continue
            chip = match.group(1)
            if chip in declared or chip in seen:
                continue
            seen.add(chip)
            errors.append(
                f"{rel}:{line_no}: includes alp/chips/{chip}.h but "
                f"board.yaml has no matching chips: entry -- add '{chip}' "
                "so the family-compatibility check and portability ring "
                "can account for it"
            )
    return errors


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


def load_board_host_families() -> dict[str, set[str]]:
    """Map board preset slug -> the set of chip-family slugs
    (aen / v2n / v2n-m1 / imx93) it hosts.

    Scraped from metadata/boards/<slug>.yaml's `hosts_som_families:`
    and translated through _VENDOR_FAMILY_TO_SLUG.  Used to decide
    whether a no-chip example's `supported_boards:` fan-out actually
    reaches more than one SoM family (issue #519) -- an entry like
    `e1m-x-evk` alone already spans two families (v2n, v2n-m1).
    """
    out: dict[str, set[str]] = {}
    for board_yaml in sorted((ROOT / "metadata" / "boards").glob("*.yaml")):
        with board_yaml.open(encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
        hosts = doc.get("hosts_som_families") or []
        if not isinstance(hosts, list):
            continue
        out[board_yaml.stem] = {
            _VENDOR_FAMILY_TO_SLUG.get(str(h), str(h)) for h in hosts
        }
    return out


def board_define_for_supported_board(board: str) -> str:
    """Return the compiler define selected by a supported_boards entry."""
    return f"ALP_BOARD_{board.upper().replace('-', '_')}"


def _iter_yaml_strings(value: object) -> Iterator[str]:
    """Yield scalar strings from parsed YAML data."""
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for item in value.values():
            yield from _iter_yaml_strings(item)
    elif isinstance(value, list):
        for item in value:
            yield from _iter_yaml_strings(item)


def _testcase_strings(testcase_yaml: pathlib.Path) -> tuple[list[str], Optional[str]]:
    try:
        with testcase_yaml.open(encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
    except yaml.YAMLError as exc:
        return [], f"testcase.yaml is not valid YAML: {exc}"
    return list(_iter_yaml_strings(doc)), None


def check_supported_board_testcases(
    example_dir: pathlib.Path,
    supported_boards: object,
) -> list[str]:
    """Verify supported_boards has explicit ALP_BOARD_* testcase variants."""
    if supported_boards in (None, []):
        return []
    if not isinstance(supported_boards, list):
        return ["supported_boards must be a list"]

    testcase_yaml = example_dir / "testcase.yaml"
    if not testcase_yaml.exists():
        return [
            "supported_boards is declared but testcase.yaml is missing -- "
            "add one Twister scenario per supported board"
        ]

    strings, parse_error = _testcase_strings(testcase_yaml)
    if parse_error:
        return [parse_error]

    testcase_text = "\n".join(strings)
    errors: list[str] = []
    for board in supported_boards:
        if not isinstance(board, str):
            errors.append(f"supported_boards entry {board!r} is not a string")
            continue
        define = board_define_for_supported_board(board)
        if define not in testcase_text:
            errors.append(
                f"supported_boards declares '{board}' but testcase.yaml has no "
                f"{define} variant"
            )
    return errors


def _no_chip_ring(family: Optional[str],
                  supported_boards: object,
                  board_host_families: dict[str, set[str]]) -> str:
    """Classify a chip-less example (issue #519).

    A `chips:`-less board.yaml carries no chip-population constraint,
    but that does NOT make it cross-family by default -- it's only
    ring1 if the example's own declarations (som.sku's family plus any
    supported_boards fan-out) actually reach >= 2 SoM families.  A
    single-SoM demo (e.g. a V2N-only eMMC/xSPI/PWM bring-up example)
    with no supported_boards stays SoM-bound -- ring3.
    """
    families: set[str] = set()
    if family:
        families.add(family)
    if isinstance(supported_boards, list):
        for board in supported_boards:
            if isinstance(board, str):
                families |= board_host_families.get(board, set())

    if len(families) >= 2:
        return "ring1-cross-family"
    if len(families) == 1:
        return "ring3-som-bound"
    # som.sku is missing/unrecognised and supported_boards resolved
    # nothing -- can't make a portability claim either way.
    return "ring-unknown"


def classify(chip_families: dict[str, list[str]],
             example_chips: list[str],
             family: Optional[str] = None,
             supported_boards: object = None,
             board_host_families: Optional[dict[str, set[str]]] = None) -> str:
    """Classify an example into ring1 / ring2 / ring3."""
    if not example_chips:
        return _no_chip_ring(family, supported_boards,
                             board_host_families or {})

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
                  som_optional: dict[str, set[str]],
                  board_host_families: Optional[dict[str, set[str]]] = None
                  ) -> tuple[str, list[str], list[str]]:
    """Return (classification, hard-error list, info-level note list)."""
    if board_host_families is None:
        board_host_families = load_board_host_families()
    board_yaml = example_dir / "board.yaml"
    if not board_yaml.exists():
        return "no-board-yaml", [], []
    with board_yaml.open(encoding="utf-8") as f:
        doc = yaml.safe_load(f) or {}

    som_sku = (doc.get("som") or {}).get("sku", "")
    family  = som_family_for_sku(som_sku)
    pinout_namespace = pinout_namespace_for_sku(som_sku)
    chips   = doc.get("chips") or []

    errors: list[str] = []
    notes:  list[str] = []

    errors.extend(check_supported_board_testcases(example_dir,
                                                  doc.get("supported_boards")))

    if family is None and som_sku:
        errors.append(
            f"unknown som.sku prefix '{som_sku}' -- can't classify; "
            f"add the prefix to _SKU_FAMILY_TABLE in {pathlib.Path(__file__).name}"
        )

    if pinout_namespace is None and som_sku:
        errors.append(
            f"unknown som.sku prefix '{som_sku}' -- can't validate the "
            f"E1M/E1M-X pinout namespace; add the prefix to "
            f"_SKU_PINOUT_TABLE in {pathlib.Path(__file__).name}"
        )

    # SDK-level block helpers live under `blocks/<name>/`, not
    # `chips/<name>/`, so they don't have a metadata/chips/<name>.yaml.
    # Skip them in the families[] check -- block helpers are
    # SoM-family-agnostic by design (they're software abstractions
    # over GPIO / PDM, not third-party ICs).
    _BLOCK_SLUGS = {"button_led", "pdm_mic"}

    if family is not None:
        for chip in chips:
            if chip in _BLOCK_SLUGS:
                continue
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

    errors.extend(check_pinout_namespace(example_dir, pinout_namespace))
    errors.extend(check_chip_includes_declared(example_dir, chips))

    ring = classify(chip_families, chips, family,
                    doc.get("supported_boards"), board_host_families)
    return ring, errors, notes


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
    board_host_families = load_board_host_families()

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
        ring, errors, notes = check_example(ex, chip_families, som_optional,
                                            board_host_families)
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
