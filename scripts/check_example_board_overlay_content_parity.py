#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: same-PCB SKU board overlays in an example must say the same thing.

Invariant
---------
Two files under the same `examples/**/boards/` directory whose names differ
only in the `alp_e1m_<sku>_` token -- e.g.
`alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay` and its
`alp_e1m_aen803_...` sibling -- must have identical devicetree / Kconfig
content, provided both SKUs are the same PCB. "Same PCB" is read from the
SoM presets, not hard-coded: two SKUs are paired when their
`metadata/e1m_modules/<SKU>.yaml` declare the same `family` AND the same
`silicon_variant` (today: E1M-AEN801/E1M-AEN803, E1M-V2N101/E1M-V2N102,
E1M-V2M101/E1M-V2M102). Such SKUs differ only by BOM population, so an
overlay delta between them is drift unless it is a population fact listed
in `ALLOWED_DELTAS` below.

Why this exists (issue #2198): `df628b6ab` (#2176) backfilled an
`alp_e1m_aen803_*` overlay for every AEN example by copying the AEN801 file.
Three AEN801 fixes landed afterwards and never reached the copies --
`00bcd9f2f` (#2051, `sdhc0` disabled because the E1M-EVK 2626-R2's
74LVC157 SDIO mux holds the SoC-side SD nets low and `sdhc_dwc_init()`
would fight it at POST_KERNEL), `4c96d7b05` (#2133, PDM
`clk-frequency-min`/`-max` for the MP34DT05TR-A) and `3ab72735b` (#2104, the
`gpio-qdec` software decoder). The AEN803 `aen-evk-demo` overlay kept
`sdhc0` enabled -- the exact driver contention #2051 removed.
`check_example_board_overlay_parity.py` passed throughout: it checks that
an overlay EXISTS for each target, not what it says.

What is compared
----------------
Comments are stripped before comparing (C-style `/* */` and `//` in
devicetree; full-line `#` in `.conf`, except Kconfig's
`# CONFIG_FOO is not set`, which is an assignment). Per-SKU comments are
legitimate and must not be forced equal: a bench result measured on an
AEN801 unit is not a claim about AEN803 (the #2176 backfill corrected two
such banner sentences for exactly that reason). Both SKU names are then
normalised to one token, whitespace is collapsed, and blank lines dropped,
so what remains is the functional content: nodes, properties, includes,
Kconfig assignments.

Pairs only -- a file with no same-PCB sibling is out of scope here (whether
it SHOULD have one is `check_example_board_overlay_parity.py`'s question).

Run locally:

    python3 scripts/check_example_board_overlay_content_parity.py

CI wires this in `pr-metadata-validate.yml` (job `validate`).
"""
from __future__ import annotations

import argparse
import difflib
import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent

# Legitimate functional deltas between same-PCB SKU overlays, keyed by
# (repo-relative path of the file that carries the line, normalised line).
# A key lets that line appear in that file without appearing in its sibling.
# Same-PCB SKUs differ ONLY by BOM population (E1M-AEN803 fits the OSPI0
# HyperRAM S80KS5122GABHM02 + xSPI NOR IS25WX256-JHLE that E1M-AEN801 does
# not), so the only admissible entries are nodes/properties describing a
# part one SKU populates and the other does not. Each entry needs a comment
# naming that part. Empty today: no paired overlay touches OSPI0 --
# aen-ospi-regcheck, the one example that does, was deliberately left
# AEN801-only by #2176 pending a maintainer call on the population split.
# ponytail: line-granular, not position-granular -- an allowed line may
# appear any number of times in that file; add hunk context if an entry
# ever needs to be that narrow.
ALLOWED_DELTAS: dict[tuple[str, str], str] = {}

_QUALIFIED_RE = re.compile(r"^alp_e1m_([a-z0-9]+)_(.+)$")
_SUFFIXES = (".overlay", ".dtsi", ".conf")
# Strings first so a `//` or `/*` inside a quoted DT string is kept.
_DTS_COMMENT_RE = re.compile(r'"(?:\\.|[^"\\\n])*"|/\*.*?\*/|//[^\n]*', re.S)
_KCONFIG_UNSET_RE = re.compile(r"#\s*CONFIG_\w+ is not set")
_MAX_SHOWN = 8


def _pcb_key(root: Path, sku: str) -> tuple[str, str] | None:
    """(family, silicon_variant) of a lowercase SKU token, or None."""
    preset = root / "metadata" / "e1m_modules" / f"E1M-{sku.upper()}.yaml"
    if not preset.is_file():
        return None
    data = yaml.safe_load(preset.read_text(encoding="utf-8")) or {}
    family, variant = data.get("family"), data.get("silicon_variant")
    return (family, variant) if family and variant else None


def normalise(text: str, suffix: str, skus: list[str]) -> list[str]:
    """Functional content of a board file: comments gone, SKUs unified."""
    if suffix == ".conf":
        text = "\n".join(
            line for line in text.splitlines()
            if not line.lstrip().startswith("#")
            or _KCONFIG_UNSET_RE.fullmatch(line.strip()))
    else:
        text = _DTS_COMMENT_RE.sub(
            lambda m: m.group(0) if m.group(0).startswith('"') else " ", text)
    text = re.sub("|".join(map(re.escape, skus)), "SKU", text,
                  flags=re.IGNORECASE)
    return [" ".join(line.split()) for line in text.splitlines()
            if line.strip()]


def find_problems(root: Path,
                  allowed: dict[tuple[str, str], str] | None = None
                  ) -> list[str]:
    allowed = ALLOWED_DELTAS if allowed is None else allowed
    groups: dict[tuple[Path, str], dict[str, Path]] = {}
    for path in sorted((root / "examples").glob("**/boards/alp_e1m_*")):
        m = _QUALIFIED_RE.match(path.name)
        if m and path.is_file() and path.suffix in _SUFFIXES:
            groups.setdefault((path.parent, m.group(2)), {})[m.group(1)] = path

    problems = []
    for members in groups.values():
        by_pcb: dict[tuple[str, str], list[str]] = {}
        for sku in sorted(members):
            key = _pcb_key(root, sku)
            if key:
                by_pcb.setdefault(key, []).append(sku)
        for skus in by_pcb.values():
            base = skus[0]
            for other in skus[1:]:
                problems += _compare(root, members[base], members[other],
                                     skus, allowed)
    return problems


def _compare(root: Path, a: Path, b: Path, skus: list[str],
             allowed: dict[tuple[str, str], str]) -> list[str]:
    rel_a, rel_b = (p.relative_to(root).as_posix() for p in (a, b))
    lines_a = normalise(a.read_text(encoding="utf-8"), a.suffix, skus)
    lines_b = normalise(b.read_text(encoding="utf-8"), b.suffix, skus)
    # autojunk off: `};` / `status = "okay";` are frequent enough in a long
    # overlay to be junked, which yields a non-minimal diff that would report
    # unchanged lines as deltas.
    matcher = difflib.SequenceMatcher(None, lines_a, lines_b, autojunk=False)
    delta = []
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag == "equal":
            continue
        delta += [f"-{ln}" for ln in lines_a[i1:i2]
                  if (rel_a, ln) not in allowed]
        delta += [f"+{ln}" for ln in lines_b[j1:j2]
                  if (rel_b, ln) not in allowed]
    if not delta:
        return []
    shown = "\n".join(f"      {d}" for d in delta[:_MAX_SHOWN])
    more = (f"\n      ... {len(delta) - _MAX_SHOWN} more"
            if len(delta) > _MAX_SHOWN else "")
    return [
        f"{rel_b}: functional content differs from same-PCB sibling {rel_a} "
        f"({len(delta)} line(s); '-' only in {a.name.split('_')[2]}, "
        f"'+' only in {b.name.split('_')[2]}):\n{shown}{more}\n"
        f"    fix: `git log -p` both files, propagate the newer, correct side "
        f"to the other; if the delta is a real BOM-population difference, "
        f"add it to ALLOWED_DELTAS in {Path(__file__).name} with a comment "
        f"naming the part."
    ]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)
    problems = find_problems(args.root)
    for problem in problems:
        print(f"ERROR: {problem}", file=sys.stderr)
    if problems:
        print(f"FAIL: {len(problems)} same-PCB overlay pair(s) drifted "
              f"(issue #2198).", file=sys.stderr)
        return 1
    print("OK: same-PCB SKU board overlays match (comments and SKU names "
          "excluded).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
