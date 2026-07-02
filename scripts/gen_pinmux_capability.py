#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Generate the per-family pin-mux capability tables from the SoM pinout TSVs.

Each family ships its pinout as TSVs under metadata/e1m_modules/<family>/,
in one of two shapes:

* E1M-pad claims (AEN): 4 columns
  (e1m_pad, e1m_function, silicon_peripheral, silicon_pad) -- plus a
  3-column raw-GPIO variant on the CC3501E map.

    metadata/e1m_modules/aen/from-alif.tsv      (Alif-Ensemble-owned pads)
    metadata/e1m_modules/aen/from-cc3501e.tsv   (TI CC3501E-owned pads)

* Pad-first peripheral maps (V2N): 2 columns (peripheral, silicon_pad).
  These don't carry the E1M edge ball, so `e1m_pad` is emitted as the
  literal "TBD" (per the no-inventing-values rule); `e1m_function` is
  taken from rows whose peripheral cell literally names an E1M net
  ("E1M PWM0" -> "PWM0") and is "TBD" for on-module / inter-chip nets.

    metadata/e1m_modules/v2n/renesas-peripheral-map.tsv  (RZ/V2N-owned pads)
    metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv         (GD32 IO-MCU pads)

The TSVs are the single source.  Tools (the `alp` CLI / the IDE) want one
structured, schema'd table per family rather than heterogeneous TSVs, so
this generator projects them into:

    metadata/pinmux/<family>.yaml   (schema: pinmux-capability-v1)

Each row becomes one pad entry: { e1m_pad, e1m_function, owner,
silicon_peripheral, silicon_pad }.  The emitted file is validated against
the schema before it is written, and a CI regen-diff gate keeps it
byte-in-sync with the TSVs (so the structured table can never drift from
the source).  Pad exclusivity is enforced separately by
scripts/check_pin_conflicts.py.

Usage:

    python3 scripts/gen_pinmux_capability.py            # regenerate in place
    python3 scripts/gen_pinmux_capability.py --check     # fail if out of sync
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import jsonschema

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("gen_pinmux_capability: PyYAML is required.  "
             "Install via `pip install pyyaml`.")

REPO = Path(__file__).resolve().parent.parent
MODULES = REPO / "metadata" / "e1m_modules"
PINMUX_DIR = REPO / "metadata" / "pinmux"
SCHEMA = REPO / "metadata" / "schemas" / "pinmux-capability-v1.schema.json"

# Per-family generation spec: which TSVs feed the table and how each maps
# onto the common pad entry.  `owner` is the on-module silicon that drives
# the pad.  `shape` selects the source column layout:
#   "e1m_claim" (default) -- (e1m_pad, e1m_function, peripheral, pad)
#   "pad_first"           -- (peripheral, pad); e1m_pad is unmappable -> "TBD"
#
# imx93 deliberately has no entry: the family ships no pinout TSV yet
# (metadata/e1m_modules/imx93/ holds only hw-revisions.yaml pending the
# IMX93RM ingestion / HW-config writeup), so there is nothing to project.
# Add it here when the TSV lands.
FAMILIES: dict[str, dict] = {
    "aen": {
        "display_name": "E1M-AEN (Alif Ensemble)",
        "sources": [
            {"file": "aen/from-alif.tsv", "owner": "alif"},
            {"file": "aen/from-cc3501e.tsv", "owner": "cc3501e"},
        ],
    },
    "v2n": {
        "display_name": "E1M-V2N (Renesas RZ/V2N + GD32 IO MCU)",
        "sources": [
            {"file": "v2n/renesas-peripheral-map.tsv", "owner": "renesas",
             "shape": "pad_first"},
            {"file": "v2n/gd32-io-mcu-map.tsv", "owner": "gd32",
             "shape": "pad_first"},
        ],
    },
}


def _read_tsv_rows(path: Path) -> list[list[str]]:
    """Return data rows (header + blank + comment lines stripped)."""
    rows: list[list[str]] = []
    with path.open(encoding="utf-8") as fi:
        for line in fi:
            s = line.rstrip("\n")
            if (not s or s.startswith("#")
                    or s.startswith("e1m_pad\t") or s.startswith("peripheral\t")):
                continue
            rows.append(s.split("\t"))
    return rows


def _pads_for_family(spec: dict) -> list[dict[str, str]]:
    """Project a family's TSVs into ordered pad entries.

    E1M-claim rows (shape "e1m_claim", the default) carry
    (e1m_pad, e1m_function, peripheral, pad).  The CC3501E map has a
    second, 3-column shape for raw GPIO pads (e1m_pad, e1m_function, gpio)
    where the on-module pin IS the function and there is no distinct
    peripheral -- those land as an empty peripheral with the GPIO as the
    silicon pad.

    Pad-first rows (shape "pad_first") carry (peripheral, pad).  The E1M
    edge ball is not in the source, so `e1m_pad` is the literal "TBD";
    `e1m_function` is projected mechanically from peripheral cells that
    literally name an E1M net ("E1M PWM0" -> "PWM0") and is "TBD" for
    on-module / inter-chip nets -- nothing is invented.
    """
    pads: list[dict[str, str]] = []
    for src in spec["sources"]:
        path = MODULES / src["file"]
        shape = src.get("shape", "e1m_claim")
        for row in _read_tsv_rows(path):
            cells = [c.strip() for c in row]
            if shape == "pad_first":
                if len(cells) < 2:
                    raise SystemExit(
                        f"gen_pinmux_capability: malformed row in "
                        f"{src['file']!r}: {row!r} (expected 2 tab-separated "
                        f"columns: peripheral, pad)")
                peripheral, pad = cells[0], cells[1]
                e1m_pad = "TBD"
                e1m_function = (peripheral[len("E1M "):]
                                if peripheral.startswith("E1M ") else "TBD")
            elif len(cells) == 3:
                e1m_pad, e1m_function, peripheral, pad = (
                    cells[0], cells[1], "", cells[2])
            elif len(cells) >= 4:
                e1m_pad, e1m_function, peripheral, pad = cells[:4]
            else:
                raise SystemExit(
                    f"gen_pinmux_capability: malformed row in {src['file']!r}: "
                    f"{row!r} (expected 3 or 4 tab-separated columns)")
            pads.append({
                "e1m_pad": e1m_pad,
                "e1m_function": e1m_function,
                "owner": src["owner"],
                "silicon_peripheral": peripheral,
                "silicon_pad": pad,
            })
    return pads


def _yaml_quote(value: str) -> str:
    """Double-quote a scalar for the flow-style emitter (stable + safe)."""
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _render(family: str, spec: dict, pads: list[dict[str, str]]) -> str:
    """Hand-roll the YAML so output is byte-stable across PyYAML versions."""
    lines = [
        "# Auto-generated by scripts/gen_pinmux_capability.py from",
        f"# metadata/e1m_modules/{family}/*.tsv.  DO NOT EDIT BY HAND -- regenerate:",
        "#     python3 scripts/gen_pinmux_capability.py",
        "# Schema: metadata/schemas/pinmux-capability-v1.schema.json",
        "#",
        "# Structured, queryable projection of the pin-mux: each E1M edge pad ->",
        "# the silicon function/peripheral that backs it, and which on-module",
        "# silicon owns it.  The TSVs remain the single source; this file is gated",
        "# byte-in-sync.  Pad exclusivity is gated by scripts/check_pin_conflicts.py.",
        "schemaVersion: pinmux-capability-v1",
        f"family: {family}",
        f"display_name: {_yaml_quote(spec['display_name'])}",
        "pads:",
    ]
    for p in pads:
        lines.append(
            "  - { "
            f"e1m_pad: {_yaml_quote(p['e1m_pad'])}, "
            f"e1m_function: {_yaml_quote(p['e1m_function'])}, "
            f"owner: {_yaml_quote(p['owner'])}, "
            f"silicon_peripheral: {_yaml_quote(p['silicon_peripheral'])}, "
            f"silicon_pad: {_yaml_quote(p['silicon_pad'])} "
            "}"
        )
    return "\n".join(lines) + "\n"


def _validate(text: str, family: str) -> None:
    """Round-trip the emitted YAML through the schema before trusting it."""
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    doc = yaml.safe_load(text)
    errors = sorted(jsonschema.Draft202012Validator(schema).iter_errors(doc),
                    key=lambda e: list(e.absolute_path))
    if errors:
        loc = "\n".join(
            f"  · {'/'.join(str(p) for p in e.absolute_path) or '<root>'}: {e.message}"
            for e in errors)
        raise SystemExit(
            f"gen_pinmux_capability: emitted {family}.yaml fails schema:\n{loc}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="fail (exit 1) if any table is out of sync with the TSVs")
    args = ap.parse_args()

    PINMUX_DIR.mkdir(parents=True, exist_ok=True)
    stale: list[str] = []
    for family, spec in FAMILIES.items():
        pads = _pads_for_family(spec)
        text = _render(family, spec, pads)
        _validate(text, family)
        out = PINMUX_DIR / f"{family}.yaml"
        if args.check:
            current = out.read_text(encoding="utf-8") if out.is_file() else ""
            if current != text:
                stale.append(family)
            else:
                print(f"OK   {out.relative_to(REPO)}  ({len(pads)} pads, in sync)")
        else:
            out.write_text(text, encoding="utf-8")
            print(f"wrote {out.relative_to(REPO)}  ({len(pads)} pads)")

    if args.check and stale:
        print(f"\ngen_pinmux_capability: {', '.join(stale)} out of sync -- run "
              "`python3 scripts/gen_pinmux_capability.py` and commit the result.",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
