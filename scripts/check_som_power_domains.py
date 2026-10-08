#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pin every SoM power-domain control pad to the pad TSVs (alp-sdk#2784).

`metadata/e1m_modules/aen/on-module-links.yaml` (v2) carries a
`power_domains:` block whose `controls[]` name the SoC pads that gate each
on-module domain (CC3501E reset/enable, Ethernet PHY reset/power-down, the
external-memory resets, the backlight enable).  Each control cites its row in
`inter-chip.tsv`, `alif-ethernet-phy.tsv` or `alif-ospi.tsv`, or a key under
`on_module_links:` in the same file.  A pad that drifts from the row it cites
would make the generated `alp,som-power` devicetree node drive the wrong SoC
pin -- on this hardware a wrong reset or enable line holds a chip in reset or
cuts its supply -- so this gate re-reads the cited row and fails on any
disagreement.

Also checked, because the generator trusts them:
  * `gpio_node` / `gpio_pin` agree with `silicon_pad` (P15_n -> lpgpio n,
    Pp_n -> gpio<p> n);
  * a pad is claimed by at most one control;
  * every `actions.*.control` and `default_action` resolve;
  * every `presence.on_module_key` resolves in at least one AEN SKU preset;
  * every `dependents[].source` signal exists in its TSV.

Run: python3 scripts/check_som_power_domains.py [--root ROOT]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import yaml

LINKS_REL = "metadata/e1m_modules/aen/on-module-links.yaml"
TSV_DIR_REL = "metadata/e1m_modules/aen"
_PAD_RE = re.compile(r"^(P\d+_\d)(?:_FLEX)?$")


def _pad(raw: str) -> str:
    """`P15_1_FLEX` -> `P15_1`; anything else is returned unchanged."""
    m = _PAD_RE.match(raw.strip())
    return m.group(1) if m else raw.strip()


def read_tsv(path: Path) -> list[dict[str, str]]:
    """Header-keyed rows; `#` comment lines and blank lines skipped."""
    rows: list[dict[str, str]] = []
    header: list[str] | None = None
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        cells = line.split("\t")
        if header is None:
            header = cells
            continue
        rows.append(dict(zip(header, cells)))
    return rows


def _tsv_pad(root: Path, name: str, signal: str) -> str | None:
    """The SoC pad the TSV row for *signal* assigns, or None if no such row."""
    for row in read_tsv(root / TSV_DIR_REL / name):
        key = row.get("signal") or row.get("phy_signal") or row.get("ospi_signal")
        if key != signal:
            continue
        pad = row.get("alif_pad")
        return _pad(pad) if pad else None
    return None


def _tsv_has(root: Path, name: str, signal: str) -> bool:
    return any((r.get("signal") or r.get("phy_signal") or r.get("ospi_signal")) == signal
               for r in read_tsv(root / TSV_DIR_REL / name))


def _expected_gpio(pad: str) -> tuple[str, int] | None:
    m = re.match(r"^P(\d+)_(\d)$", pad)
    if not m:
        return None
    port, pin = int(m.group(1)), int(m.group(2))
    return ("lpgpio" if port == 15 else f"gpio{port}"), pin


def _dig(doc: object, dotted: str) -> object:
    for part in dotted.split("."):
        doc = doc.get(part) if isinstance(doc, dict) else None
    return doc


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    links_path = root / LINKS_REL
    if not links_path.is_file():
        return [f"{LINKS_REL}: missing"]
    doc = yaml.safe_load(links_path.read_text(encoding="utf-8"))
    domains = doc.get("power_domains")
    if not isinstance(domains, dict) or not domains:
        return [f"{LINKS_REL}: no power_domains: block"]
    links = doc.get("on_module_links") or {}

    presets = [yaml.safe_load(p.read_text(encoding="utf-8")) or {}
               for p in sorted((root / "metadata/e1m_modules").glob("E1M-AEN*.yaml"))]

    claimed: dict[str, str] = {}
    for name, dom in domains.items():
        where = f"{LINKS_REL}: power_domains.{name}"
        signals = set()
        for ctl in dom.get("controls") or []:
            sig = ctl.get("signal", "?")
            signals.add(sig)
            pad = ctl.get("silicon_pad", "?")
            src = ctl.get("source") or {}
            fname = src.get("file")
            if fname == "on-module-links.yaml":
                node = _dig(links, src.get("key", ""))
                cited = node.get("silicon_pad") if isinstance(node, dict) else None
                if cited is None:
                    problems.append(
                        f"{where}.{sig}: source key {src.get('key')!r} has no "
                        "silicon_pad under on_module_links")
                elif _pad(cited) != pad:
                    problems.append(
                        f"{where}.{sig}: silicon_pad {pad} but on_module_links."
                        f"{src['key']} says {cited}")
            elif fname:
                if not (root / TSV_DIR_REL / fname).is_file():
                    problems.append(f"{where}.{sig}: source file {fname} missing")
                    continue
                row_pad = _tsv_pad(root, fname, src.get("signal", ""))
                if row_pad is None:
                    problems.append(
                        f"{where}.{sig}: no row {src.get('signal')!r} with a pad "
                        f"in {fname}")
                elif row_pad != pad:
                    problems.append(
                        f"{where}.{sig}: silicon_pad {pad} but {fname} row "
                        f"{src['signal']} says {row_pad}")
            else:
                problems.append(f"{where}.{sig}: no source")
            exp = _expected_gpio(pad)
            if exp is None:
                problems.append(f"{where}.{sig}: silicon_pad {pad!r} is not Pp_n")
            elif (ctl.get("gpio_node"), ctl.get("gpio_pin")) != exp:
                problems.append(
                    f"{where}.{sig}: {pad} is {exp[0]} pin {exp[1]}, metadata "
                    f"says {ctl.get('gpio_node')} pin {ctl.get('gpio_pin')}")
            if pad in claimed:
                problems.append(
                    f"{where}.{sig}: pad {pad} already claimed by {claimed[pad]}")
            claimed[pad] = f"{name}.{sig}"

        actions = dom.get("actions") or {}
        if dom.get("default_action") not in actions:
            problems.append(f"{where}: default_action {dom.get('default_action')!r} "
                            "is not in actions:")
        for aname, act in actions.items():
            ref = act.get("control")
            if ref is not None and ref not in signals:
                problems.append(f"{where}.actions.{aname}: control {ref!r} is not "
                                "one of the domain's controls")

        for dep in dom.get("dependents") or []:
            src = dep.get("source") or {}
            if src.get("file") and not _tsv_has(root, src["file"], src.get("signal", "")):
                problems.append(f"{where}.dependents: no row {src.get('signal')!r} "
                                f"in {src['file']}")

        key = (dom.get("presence") or {}).get("on_module_key")
        if key and not any(_dig(p.get("on_module") or {}, key) is not None for p in presets):
            problems.append(f"{where}: presence.on_module_key {key!r} resolves in "
                            "no E1M-AEN* SoM preset")
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = ap.parse_args(argv)
    problems = find_problems(args.root)
    for p in problems:
        print(f"FAIL {p}", file=sys.stderr)
    if problems:
        print("fix the power_domains control (or the TSV row it cites) so the "
              "pad agrees", file=sys.stderr)
        return 1
    print("OK   SoM power-domain control pads match the pad TSVs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
