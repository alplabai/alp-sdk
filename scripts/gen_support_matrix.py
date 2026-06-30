#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Generate docs/peripheral-support-matrix.md from the single-source metadata.

Each E1M SoM preset (metadata/e1m_modules/E1M-*.yaml) names its on-module
silicon via a `silicon:` triple-colon ref (e.g. `alif:ensemble:e8`).  That
ref resolves to a SoC spec (metadata/socs/<vendor>/<family>/<part>.json)
whose `peripherals:` integer counts and `npus:` array are the zero-drift
source of truth for what hardware each module exposes.

This generator projects those facts into a *presence* matrix: one row per
SoM SKU, one column per peripheral CLASS, cell = present iff the SoC
declares a positive count for that class (npus[] non-empty for the NPU
column).

Scope is PRESENCE only.  Driver tier (Tier-1/2/3) and GA / stub maturity
are NOT structured metadata -- tier lives in free-text driver `.c` headers --
so they are intentionally out of scope here.  The hand-maintained maturity
view lives in docs/os-support-matrix.md.

The peripheral-class projection mirrors scripts/gen_soc_caps.py's CAPS
list, but its predicates normalise across SoC key-naming variants (e.g.
`ethernet` vs `ethernet_1g`, `uart` vs `scif`) so presence is captured
uniformly across vendors.

Usage:

    python3 scripts/gen_support_matrix.py            # regenerate in place
    python3 scripts/gen_support_matrix.py --check     # fail if out of sync
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("gen_support_matrix: PyYAML is required.  "
             "Install via `pip install pyyaml`.")

REPO = Path(__file__).resolve().parent.parent
MODULES = REPO / "metadata" / "e1m_modules"
SOCS = REPO / "metadata" / "socs"
OUT = REPO / "docs" / "peripheral-support-matrix.md"

PRESENT = "✅"   # white heavy check mark
ABSENT = "—"    # em dash


def _has(soc: dict, *keys: str) -> bool:
    """True iff the SoC declares a positive count for any exact key."""
    p = soc.get("peripherals", {}) or {}
    return any(int(p.get(k, 0) or 0) > 0 for k in keys)


def _has_prefix(soc: dict, *prefixes: str) -> bool:
    """True iff any integer peripheral key starting with a prefix is > 0.

    Captures vendor key-naming variants under one class (e.g. `ethernet`
    + `ethernet_1g`, every `usb_*`, every `timer_*`).
    """
    p = soc.get("peripherals", {}) or {}
    return any(isinstance(v, int) and v > 0 and k.startswith(prefixes)
               for k, v in p.items())


def soc_npus(soc: dict) -> list:
    return soc.get("npus") or []


# Peripheral CLASS columns, in a fixed (deterministic) canonical order.
# Each entry is (label, predicate(soc) -> bool).  Modelled on
# scripts/gen_soc_caps.py CAPS; predicates normalise the SoC key-naming
# variants so a class is PRESENT iff the SoC declares >= 1 instance of it.
PERIPHERAL_CLASSES: list[tuple[str, "callable"]] = [
    ("I2C", lambda s: _has(s, "i2c", "i2c_lp")),
    ("I3C", lambda s: _has(s, "i3c", "i3c_lp")),
    ("SPI", lambda s: _has(s, "spi", "spi_lp", "qspi")),
    ("UART", lambda s: _has(s, "uart", "uart_lp", "scif")),
    ("I2S", lambda s: _has(s, "i2s", "i2s_lp")),
    ("PDM", lambda s: _has(s, "pdm", "pdm_lp")),
    ("CAN", lambda s: _has(s, "can", "can_fd")),
    ("ADC", lambda s: _has_prefix(s, "adc_")),
    ("DAC", lambda s: _has_prefix(s, "dac_")),
    ("PWM", lambda s: _has(s, "pwm")),
    ("Timer", lambda s: _has_prefix(s, "timer_")),
    ("Quadrature Encoder", lambda s: _has(s, "encoder_quadrature")),
    ("RTC", lambda s: _has(s, "rtc")),
    ("Watchdog", lambda s: _has(s, "watchdog")),
    ("Ethernet", lambda s: _has_prefix(s, "ethernet")),
    ("USB", lambda s: _has_prefix(s, "usb_")),
    ("SDIO/eMMC", lambda s: _has_prefix(s, "sdio")),
    ("PCIe", lambda s: _has_prefix(s, "pcie")),
    ("MIPI CSI", lambda s: _has(s, "mipi_csi2")),
    ("MIPI DSI", lambda s: _has(s, "mipi_dsi")),
    ("NPU", lambda s: bool(soc_npus(s))),
]


def load_socs() -> dict[str, dict]:
    """Map each SoC `ref` -> parsed SoC spec JSON."""
    by_ref: dict[str, dict] = {}
    for path in sorted(SOCS.rglob("*.json")):
        soc = json.loads(path.read_text(encoding="utf-8"))
        ref = soc.get("ref")
        if ref:
            by_ref[ref] = soc
    return by_ref


def load_modules() -> list[tuple[str, str]]:
    """Return sorted [(sku, silicon_ref)] for every E1M SoM preset."""
    mods: list[tuple[str, str]] = []
    for path in sorted(MODULES.glob("E1M-*.yaml")):
        doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        sku = doc.get("sku") or path.stem
        ref = doc.get("silicon")
        if not ref:
            raise SystemExit(
                f"gen_support_matrix: {path.name} has no `silicon:` ref")
        mods.append((sku, ref))
    return sorted(mods)


def build_rows(mods: list[tuple[str, str]],
               socs: dict[str, dict]) -> list[tuple[str, str, dict, list[bool]]]:
    """Resolve each SoM to its SoC and compute the per-class presence row."""
    rows: list[tuple[str, str, dict, list[bool]]] = []
    for sku, ref in mods:
        soc = socs.get(ref)
        if soc is None:
            raise SystemExit(
                f"gen_support_matrix: {sku} references silicon {ref!r} with "
                f"no matching metadata/socs/**.json (ref field)")
        flags = [pred(soc) for _label, pred in PERIPHERAL_CLASSES]
        rows.append((sku, ref, soc, flags))
    return rows


def render(rows: list[tuple[str, str, dict, list[bool]]]) -> str:
    """Hand-roll byte-stable Markdown (no third-party formatter dependency)."""
    # A class column is shown only if at least one SoM supports it -- this
    # drops all-absent columns (e.g. PWM, which no current SoM SoC declares)
    # without making the output non-deterministic: the canonical order above
    # is preserved.
    keep = [i for i in range(len(PERIPHERAL_CLASSES))
            if any(row[3][i] for row in rows)]
    labels = [PERIPHERAL_CLASSES[i][0] for i in keep]

    lines: list[str] = [
        "<!-- AUTO-GENERATED by scripts/gen_support_matrix.py "
        "— DO NOT EDIT; regenerate -->",
        "<!--",
        "     Regenerate after editing metadata/e1m_modules/E1M-*.yaml or",
        "     metadata/socs/**.json:",
        "         python3 scripts/gen_support_matrix.py",
        "     A CI gate (.github/workflows/pr-generated-files.yml) fails the",
        "     PR if this file drifts from the metadata.",
        "-->",
        "",
        "# Peripheral Support Matrix",
        "",
        "One row per E1M SoM SKU, one column per peripheral class.  A cell is",
        f"{PRESENT} when the module's on-module silicon declares at least one",
        f"instance of that class, {ABSENT} when it does not.",
        "",
        "This table is **auto-generated** from the single-source hardware",
        "metadata: each SoM preset (`metadata/e1m_modules/E1M-*.yaml`) names",
        "its silicon via a `silicon:` ref, which resolves to a SoC spec",
        "(`metadata/socs/<vendor>/<family>/<part>.json`).  The SoC's",
        "`peripherals:` counts and `npus:` array are the zero-drift source of",
        "truth -- edit the metadata, not this file.",
        "",
        "**Scope: presence only.**  Driver maturity (tier, GA / stub) is not",
        "structured metadata and is tracked separately in",
        "[os-support-matrix.md](os-support-matrix.md); it is intentionally out",
        "of scope here.",
        "",
        f"> A blank ({ABSENT}) means the class is **not declared** in the SoC",
        "> metadata.  Where a SoC's peripheral counts have not yet been fully",
        "> ingested the row is sparse; cells fill in automatically once the",
        "> metadata lands (the CI drift gate keeps this file in sync).",
        "",
    ]

    header = ["SoM SKU", "Silicon", *labels]
    sep = ["---", "---", *([":---:"] * len(labels))]
    lines.append("| " + " | ".join(header) + " |")
    lines.append("| " + " | ".join(sep) + " |")
    for sku, ref, _soc, flags in rows:
        cells = [PRESENT if flags[i] else ABSENT for i in keep]
        lines.append("| " + " | ".join([sku, f"`{ref}`", *cells]) + " |")

    lines.append("")
    lines.append(f"Legend: {PRESENT} present · {ABSENT} absent (not "
                 "declared in metadata).")
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="fail (exit 1) if the matrix is out of sync with "
                         "the metadata")
    args = ap.parse_args()

    socs = load_socs()
    mods = load_modules()
    rows = build_rows(mods, socs)
    text = render(rows)

    if args.check:
        current = OUT.read_text(encoding="utf-8") if OUT.is_file() else ""
        if current != text:
            print("gen_support_matrix: docs/peripheral-support-matrix.md is "
                  "out of sync -- run `python3 scripts/gen_support_matrix.py` "
                  "and commit the result.", file=sys.stderr)
            return 1
        print(f"OK   {OUT.relative_to(REPO)}  ({len(rows)} SoMs, in sync)")
        return 0

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUT.relative_to(REPO)}  ({len(rows)} SoMs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
