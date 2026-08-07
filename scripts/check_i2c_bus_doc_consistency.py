#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: doc/example prose that names a known on-module I2C part next to
an I2C bus name must agree with that part's bus in the owning SoM
preset's `on_module.i2c_devices:` block -- the machine-readable source
of truth `metadata/e1m_modules/E1M-AEN801.yaml` (and its five AEN
siblings) gained in #1270.

Before #1270, "which bus is this part on" existed only in prose, spread
across docs/bring-up-aen.md, docs/soms/aen.md, docs/troubleshooting.md,
docs/cc3501e-bridge.md, several examples/aen/* READMEs and their
src/main.c comments -- with no source of truth to check any of them
against. Four consecutive wave-4 doc passes each corrected one bus
assignment and broke another; this is the cross-check #1270 asked for,
mirroring what check_pin_conflicts.py does for silicon pads.

Scope -- a NARROW gate, deliberately, per #1270's own "even a narrow
version ... would have caught every defect above" framing:

* Ground truth is built ONLY from `on_module.i2c_devices` blocks across
  every `metadata/e1m_modules/E1M-*.yaml` preset. A chip carrying a
  DIFFERENT bus assignment across two presets that both declare it is
  ambiguous ground truth and is silently excluded from checking --
  never guessed at (`_chip_truth()`).
* A doc LINE is only checked when it names exactly ONE bus (by the
  bus key's own literal spelling or one of the human-readable aliases
  in `_BUS_ALIASES`) -- a line naming two buses is typically a
  deliberate contrast ("I2C2, NOT LPI2C0") and is skipped rather than
  risk a false positive on it.
* `_BUS_ALIASES` is a small hand-maintained table, not derived from
  `bus_pads:` prose -- verified by grep against the tree at write time
  to carry no cross-family collision (`I2C2`/`LPI2C0` are AEN-only
  spellings today; `ALP_E1M_X_I2C2` is a different, unrelated E1M-X
  port-identity namespace and is never matched here because those
  lines never also name one of these on-module parts). A new family's
  own physical-bus spelling needs a matching alias added here, not a
  generic regex.
* Board-side (carrier) parts recorded in `metadata/boards/*.yaml`
  `populated:` blocks are OUT OF SCOPE -- this gate only checks
  on-module parts declared in an `i2c_devices:` block. A prose line
  naming a carrier-side-only part (e.g. the AEN EVK's TCAL9538 /
  INA236 / BMP581) is never flagged, even if it disagrees with reality.

Run locally:

    python3 scripts/check_i2c_bus_doc_consistency.py

CI wires this in `pr-metadata-validate.yml`.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent
MODULES_DIR = REPO / "metadata" / "e1m_modules"
CHIPS_DIR = REPO / "metadata" / "chips"

# Human-readable spellings docs use for a machine `i2c_devices` bus key,
# beyond the key's own literal spelling (`BRD_I2C` / `E1M_I2C0`, always
# matched too). See the module docstring's "Scope" section for the
# collision check this table relies on.
_BUS_ALIASES: dict[str, set[str]] = {
    "brd_i2c": {"LPI2C0"},
    "e1m_i2c0": {"I2C2"},
}

# Chip-id name parts too generic to serve as a standalone doc alias on
# their own (role words that would collide with unrelated prose
# anywhere in the docs tree, e.g. "eeprom" alone).
_GENERIC_PARTS = {"eeprom", "chip", "sensor", "clock", "clk"}

_NON_ALNUM_RE = re.compile(r"[^A-Za-z0-9]+")

DOC_GLOBS = ("docs/**/*.md", "examples/**/*.md", "examples/**/*.c")


def _squash(text: str) -> str:
    """Uppercase, alphanumeric-only projection, used so a chip name
    matches regardless of doc punctuation (`RV-3028-C7` and the
    metadata's `rv3028c7` chip-id both squash to `RV3028C7`)."""
    return _NON_ALNUM_RE.sub("", text).upper()


def _chip_aliases(chip_id: str, mpn_population: list[str]) -> set[str]:
    """Doc-name candidates for one chip: the full squashed chip-id, its
    non-generic squashed underscore-parts (e.g. `24c128` out of
    `eeprom_24c128`), and every squashed `mpn_population` entry."""
    aliases = {_squash(chip_id)}
    for part in chip_id.split("_"):
        squashed = _squash(part)
        if len(squashed) >= 4 and part not in _GENERIC_PARTS:
            aliases.add(squashed)
    for mpn in mpn_population:
        squashed = _squash(mpn)
        if squashed:
            aliases.add(squashed)
    return {a for a in aliases if a}


def _load_chip_mpns(root: Path) -> dict[str, list[str]]:
    mpns: dict[str, list[str]] = {}
    chips_dir = root / "metadata" / "chips"
    if not chips_dir.is_dir():
        return mpns
    for f in sorted(chips_dir.glob("*.yaml")):
        with f.open(encoding="utf-8") as fh:
            data = yaml.safe_load(fh) or {}
        cid = data.get("chip_id")
        if cid:
            mpns[cid] = [str(m) for m in data.get("mpn_population", [])]
    return mpns


def _chip_truth(root: Path) -> tuple[dict[str, str], dict[str, str]]:
    """Return (chip_id -> bus_key, chip_id -> preset path it came from)
    for every chip whose on-module bus assignment is IDENTICAL across
    every SoM preset that declares it. A chip seen on two different
    buses across presets is ambiguous ground truth and is dropped from
    both dicts -- never guessed at."""
    modules_dir = root / "metadata" / "e1m_modules"
    seen: dict[str, set[str]] = {}
    source: dict[str, str] = {}
    if not modules_dir.is_dir():
        return {}, {}
    for preset in sorted(modules_dir.glob("E1M-*.yaml")):
        with preset.open(encoding="utf-8") as f:
            data = yaml.safe_load(f) or {}
        buses = ((data.get("on_module") or {}).get("i2c_devices")) or {}
        for bus_key, bus in buses.items():
            for dev in (bus or {}).get("devices", []) or []:
                cid = dev.get("chip")
                if not cid:
                    continue
                seen.setdefault(cid, set()).add(bus_key)
                source.setdefault(cid, str(preset.relative_to(root).as_posix()))
    truth = {cid: next(iter(buses)) for cid, buses in seen.items() if len(buses) == 1}
    return truth, {cid: source[cid] for cid in truth}


def _bus_pattern(bus_key: str) -> re.Pattern[str]:
    names = {bus_key.upper()} | _BUS_ALIASES.get(bus_key, set())
    alt = "|".join(re.escape(n) for n in sorted(names, key=len, reverse=True))
    return re.compile(rf"\b(?:{alt})\b", re.IGNORECASE)


def find_problems(root: Path) -> list[str]:
    """Pure check: returns human-readable problem strings, empty when
    every checked doc/example line agrees with the i2c_devices ground
    truth (or names no ground-truth chip / no single bus)."""
    chip_truth, chip_source = _chip_truth(root)
    if not chip_truth:
        return []

    chip_mpns = _load_chip_mpns(root)
    chip_aliases = {
        cid: _chip_aliases(cid, chip_mpns.get(cid, [])) for cid in chip_truth
    }
    bus_patterns = {bus: _bus_pattern(bus) for bus in sorted(set(chip_truth.values()))}

    problems: list[str] = []
    for pattern in DOC_GLOBS:
        for path in sorted(root.glob(pattern)):
            rel = path.relative_to(root).as_posix()
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            for lineno, line in enumerate(text.splitlines(), start=1):
                mentioned_buses = {
                    bus for bus, pat in bus_patterns.items() if pat.search(line)
                }
                if len(mentioned_buses) != 1:
                    continue
                (doc_bus,) = mentioned_buses
                squashed_line = _squash(line)
                for cid, aliases in chip_aliases.items():
                    true_bus = chip_truth[cid]
                    if true_bus == doc_bus:
                        continue
                    if any(alias in squashed_line for alias in aliases):
                        problems.append(
                            f"{rel}:{lineno}: names '{cid}' next to bus "
                            f"'{doc_bus}', but {chip_source[cid]} "
                            f"on_module.i2c_devices puts '{cid}' on "
                            f"'{true_bus}' -- fix the doc or the "
                            f"metadata: {line.strip()!r}"
                        )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    problems = find_problems(args.root)

    if problems:
        print(
            "i2c-bus-doc-consistency: doc/example prose disagrees with "
            "on_module.i2c_devices metadata:",
            file=sys.stderr,
        )
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        print(
            f"\ni2c-bus-doc-consistency: {len(problems)} problem(s) -- failing.",
            file=sys.stderr,
        )
        return 1

    print("i2c-bus-doc-consistency: OK (no doc/example bus-name mismatch found).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
