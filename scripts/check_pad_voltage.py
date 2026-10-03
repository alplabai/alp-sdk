#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Pad-voltage gate: a carrier must not put a 3.3 V signal, or enable a bus, on
an SoC pad that is not 3.3 V tolerant without declaring a level shifter.

The tolerance is a SoC fact (`pad_tolerance` in metadata/socs/**/*.json, e.g.
RZ/V2N P90-P92, P2x and PBx, hardware manual R01UH1071EJ0120 section
4.2.3.1.1 Note 1).  A board preset declares what it puts on those pads in
`pad_levels:` (`signal_v`, optional `level_shifter`).  Failures:

  * a `pad_levels` entry on a non-tolerant pad with `signal_v` above the
    SoC `max_signal_v` and no `level_shifter`;
  * an `e1m_routes.buses` entry that enables a function whose SoM pad
    (metadata/pinmux/*.yaml, owner renesas, E1M pad not TBD) is non-tolerant
    and has no `pad_levels` entry.

ponytail: bus -> pad resolution is by E1M function prefix (E1M_X_SPI0 ->
`SPI0_*`, falling back to the bare class `I3C_*`); gpio routes are not
resolved.  Add that when a carrier routes a raw GPIO onto such a pad.

Run: python3 scripts/check_pad_voltage.py [--root ROOT]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import yaml


def _load_yaml(p: Path):
    return yaml.safe_load(p.read_text(encoding="utf-8")) or {}


def _tolerance(root: Path):
    """{"<vendor>-<family>": [(compiled non-tolerant regexes, max_signal_v)]}."""
    out: dict[str, list] = {}
    for p in sorted((root / "metadata/socs").glob("*/*/*.json")):
        t = json.loads(p.read_text(encoding="utf-8")).get("pad_tolerance")
        if t:
            out.setdefault(f"{p.parent.parent.name}-{p.parent.name}", []).append(
                ([re.compile(r) for r in t["non_33v_tolerant_pads"]], t["max_signal_v"]))
    return out


def _board_tolerance(tol: dict, families: list[str]) -> list:
    """Tolerance of the SoCs a board hosts (a SoM family `renesas-rzv2n-deepx`
    runs the `renesas-rzv2n` host SoC)."""
    return [t for key, ts in tol.items()
            if any(f == key or f.startswith(key + "-") for f in families) for t in ts]


def _intolerant(pad: str, tol) -> float | None:
    for pats, maxv in tol:
        if any(r.fullmatch(pad) for r in pats):
            return maxv
    return None


def _route_pads(inst: str, rows: list[dict]) -> list[dict]:
    base = re.sub(r"^E1M(_X)?_", "", inst)
    for pref in (base + "_", re.sub(r"\d+$", "", base) + "_"):
        hit = [r for r in rows if str(r.get("e1m_function", "")).startswith(pref)]
        if hit:
            return hit
    return []


def find_problems(root: Path) -> list[str]:
    all_tol = _tolerance(root)
    rows = []
    for p in sorted((root / "metadata/pinmux").glob("*.yaml")):
        for r in _load_yaml(p).get("pads", []):
            if r.get("owner") == "renesas" and r.get("e1m_pad") not in (None, "TBD"):
                rows.append(r)
    problems = []
    for bp in sorted((root / "metadata/boards").glob("*.yaml")):
        b = _load_yaml(bp)
        rel = bp.relative_to(root).as_posix()
        declared = {}
        tol = _board_tolerance(all_tol, b.get("hosts_som_families") or [])
        for lv in b.get("pad_levels") or []:
            maxv = _intolerant(lv["pad"], tol)
            declared[lv["pad"]] = lv
            if maxv is not None and lv["signal_v"] > maxv and not lv.get("level_shifter"):
                problems.append(
                    f"{rel}: pad_levels {lv['pad']} signal_v {lv['signal_v']} V exceeds the "
                    f"{maxv} V this pad tolerates; declare `level_shifter:` or lower signal_v")
        if not tol:
            continue
        for e in (b.get("e1m_routes") or {}).get("buses", []):
            for r in _route_pads(e["e1m"], rows):
                pad = r["silicon_pad"]
                if _intolerant(pad, tol) is not None and pad not in declared:
                    problems.append(
                        f"{rel}: {e['e1m']} enables {r['e1m_function']} on {pad}, which is not 3.3 V "
                        f"tolerant; add a `pad_levels:` entry for {pad} (signal_v or level_shifter)")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    problems = find_problems(ap.parse_args().root)
    for p in problems:
        print(f"ERROR: {p}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
