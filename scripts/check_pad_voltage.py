#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Pad-voltage gate: a carrier must not drive a 3.3 V level onto an SoM pad whose
SoC pad is not 3.3 V tolerant without declaring a level shifter.

The tolerance is a SoC fact (`pad_tolerance` in metadata/socs/**/*.json, e.g.
RZ/V2N P90-P92, P2x and PBx, hardware manual R01UH1071EJ0120 section
4.2.3.1.1 Note 1).  The carrier never names SoC pads: it declares what it
drives per E1M route in `pad_levels:` (`e1m: E1M_X_SPI0`, `signal_v`, optional
`level_shifter`), and this gate resolves each route through every SoM the
board hosts:

  hosts_som_families -> SoM presets (family, silicon) -> host SoC JSON
  (`pad_tolerance`; the SoC vendor is the `owner` of the host-SoC pinmux rows)
  and the pinmux table whose `som_families` lists that family
  (metadata/pinmux/<family>.yaml) -> the rows an `e1m_routes` entry enables
  (any route class, gpio included) -> their `silicon_pad`.

Failures, for every hosted SoM whose SoC declares `pad_tolerance`:

  * a route resolves to a non-tolerant pad and has no `pad_levels` entry;
  * a `pad_levels` entry has `signal_v` above the SoC `max_signal_v` and no
    `level_shifter`;
  * a `pad_levels` entry names an E1M route the board does not declare.

A route class `check_e1m_route_capability._expected_functions` does not know
(e.g. a sideband on an I2S pad) resolves to no pads and is skipped; that gate
owns it.

Run: python3 scripts/check_pad_voltage.py [--root ROOT]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
from alp_project_loader import resolve_soc_path, split_silicon_ref  # noqa: E402
from check_e1m_route_capability import _expected_functions, _v2n_function_aliases  # noqa: E402


def _load_yaml(p: Path):
    return yaml.safe_load(p.read_text(encoding="utf-8")) or {}


def _route_refs(node) -> set[str]:
    """Every `e1m: E1M_*` route a board declares (all classes)."""
    out: set[str] = set()
    if isinstance(node, dict):
        v = node.get("e1m")
        if isinstance(v, str) and v.startswith("E1M_"):
            out.add(v)
        for x in node.values():
            out |= _route_refs(x)
    elif isinstance(node, list):
        for x in node:
            out |= _route_refs(x)
    return out


def _rows_for(ref: str, rows: list[dict]) -> list[dict]:
    expected = _expected_functions(ref)
    if not expected:
        return []
    out = []
    for r in rows:
        funcs = {r["e1m_function"], *_v2n_function_aliases(r.get("silicon_peripheral", ""))}
        if expected[0].startswith("__PREFIX__"):
            hit = any(f.startswith(expected[0][len("__PREFIX__"):]) for f in funcs)
        else:
            hit = bool(funcs & set(expected))
        if hit:
            out.append(r)
    return out


def find_problems(root: Path) -> list[str]:
    soms: dict[str, set[str]] = {}  # SoM family -> host silicon refs
    for p in sorted((root / "metadata/e1m_modules").glob("E1M-*.yaml")):
        d = _load_yaml(p)
        if d.get("family") and d.get("silicon"):
            soms.setdefault(d["family"], set()).add(d["silicon"])
    tables = [_load_yaml(p) for p in sorted((root / "metadata/pinmux").glob("*.yaml"))]

    problems: list[str] = []
    for bp in sorted((root / "metadata/boards").glob("*.yaml")):
        b = _load_yaml(bp)
        rel = bp.relative_to(root).as_posix()
        routes = _route_refs(b.get("e1m_routes") or {})
        declared = {lv["e1m"]: lv for lv in b.get("pad_levels") or []}
        for ref in sorted(set(declared) - routes):
            problems.append(f"{rel}: pad_levels names {ref}, which e1m_routes does not declare")
        families = b.get("hosts_som_families") or []
        for table in tables:
            fams = [f for f in families if f in (table.get("som_families") or [])]
            for silicon in sorted({s for f in fams for s in soms.get(f, ())}):
                vendor = split_silicon_ref(silicon)[0]
                soc = resolve_soc_path(silicon, root / "metadata")
                tol = json.loads(soc.read_text(encoding="utf-8")).get("pad_tolerance")
                if not tol:
                    continue
                pats = [re.compile(r) for r in tol["non_33v_tolerant_pads"]]
                # ponytail: the host SoC owns the rows whose `owner` equals its vendor name.
                rows = [r for r in table["pads"] if r["owner"] == vendor]
                for ref in sorted(routes):
                    pads = sorted({r["silicon_pad"] for r in _rows_for(ref, rows)
                                   if any(x.fullmatch(r["silicon_pad"]) for x in pats)})
                    if not pads:
                        continue
                    lv = declared.get(ref)
                    where = f"{ref} on {silicon} pad(s) {', '.join(pads)}"
                    if lv is None:
                        problems.append(
                            f"{rel}: {where} is not 3.3 V tolerant; add a `pad_levels:` entry for "
                            f"{ref} (signal_v or level_shifter)")
                    elif lv["signal_v"] > tol["max_signal_v"] and not lv.get("level_shifter"):
                        problems.append(
                            f"{rel}: pad_levels {where}: signal_v {lv['signal_v']} V exceeds the "
                            f"{tol['max_signal_v']} V these pads tolerate; declare `level_shifter:` "
                            f"or lower signal_v")
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
