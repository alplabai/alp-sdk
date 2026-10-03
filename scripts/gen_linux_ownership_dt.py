#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Generate the Linux devicetree fragment for per-product core ownership on the
RZ/V2N SoM family, so the A55 tree follows the same metadata the CM33 board
tree and the planner follow (#2660).

Output (committed, DO NOT EDIT BY HAND):

  meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-ownership.dtsi

Inputs (nothing is restated here; every value is read):

  metadata/e1m_modules/v2n/core-ownership.yaml   assignable: per E1M instance,
                                                 default core, rows, hw_blocked,
                                                 soc_instance
  metadata/e1m_modules/v2n/supervisor-links.yaml enabled links (the GD32 SCI7
                                                 link) are always CM33-owned
  metadata/socs/renesas/rzv2n/n44.json           linux_dt[soc_instance]: Linux
                                                 node label, PFC function codes
                                                 (pinmux), CPG clock names

For every assignable instance at its SoM default core:

  * owned by the A55 and not hw_blocked: the node is enabled with a pinctrl
    group built from the rows' pads + the SoC linux_dt PFC codes -- but ONLY if
    the SoC carries a function code for every row.  A missing code is a GAP
    comment, never a guessed number, and the node stays as the vendor dtsi has it;
  * hw_blocked: left untouched (disabled) and the reason is quoted;
  * owned by the CM33: left untouched for Linux.

The CPG node also gets `renesas,cm33-owned-clocks` (honoured by the
0001-clk-renesas-rzv2h-cpg-cm33-owned-clocks kernel patch): the `cpg_clocks`
of every CM33-owned resource -- the enabled supervisor links (SCI7 to the
GD32, always) and each assignable instance owned by the CM33 -- so Linux's
clk_disable_unused never stops a peripheral the other core is using.

`--vendor-dtsi <r9a09g056.dtsi>` additionally verifies that every node label
(and CAN-FD channel node) the fragment references exists in the vendor SoC
dtsi, so the fragment never names a node the kernel tree lacks.

Usage:

    python3 scripts/gen_linux_ownership_dt.py            # (re)write the output
    python3 scripts/gen_linux_ownership_dt.py --check    # exit 1 on drift
    python3 scripts/gen_linux_ownership_dt.py --vendor-dtsi PATH
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))

import yaml  # noqa: E402

from alp_orchestrate.ownership import (  # noqa: E402
    instance_pfc, load_ownership_doc, resolve_ownership)

OUT = Path("meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-ownership.dtsi")
SOC = Path("metadata/socs/renesas/rzv2n/n44.json")
SRC = "metadata/e1m_modules/v2n/core-ownership.yaml"
LINKS = "metadata/e1m_modules/v2n/supervisor-links.yaml"


class GenError(Exception):
    pass


def _load(root: Path) -> tuple[dict, dict, dict]:
    doc = load_ownership_doc(root / "metadata", "v2n")
    if not doc or not doc.get("assignable"):
        raise GenError(f"{SRC} has no assignable: block")
    soc = json.loads((root / SOC).read_text(encoding="utf-8"))
    links = yaml.safe_load((root / LINKS).read_text(encoding="utf-8"))["supervisor_links"]
    return doc, soc, links


def cm33_clocks(doc: dict, soc: dict, links: dict, own: dict[str, str]) -> list[str]:
    """CPG clock names to keep on for the CM33: enabled supervisor links +
    CM33-owned assignable instances (a hw_blocked instance is never enabled)."""
    linux_dt = soc.get("linux_dt") or {}
    by_label = {v["label"]: v for v in linux_dt.values() if v.get("cpg_clocks")}
    clocks: list[str] = []

    def add(names: list[str]) -> None:
        clocks.extend(n for n in names if n not in clocks)

    for name, link in sorted(links.items()):
        if link.get("status") != "enabled":
            continue
        ent = by_label.get(link["dt_label"])
        if ent is None:
            raise GenError(f"supervisor link {name}: no cpg_clocks for &{link['dt_label']} in "
                           f"{SOC.as_posix()} linux_dt")
        add(ent["cpg_clocks"])
    for inst, e in sorted(doc["assignable"].items()):
        if own[inst] == "m33" and not e.get("hw_blocked"):
            ent = linux_dt.get(e.get("soc_instance")) or {}
            if not ent.get("cpg_clocks"):
                raise GenError(f"assignable.{inst}: owned by m33 but linux_dt.{e.get('soc_instance')} "
                               "has no cpg_clocks")
            add(ent["cpg_clocks"])
    return clocks


def _group(inst: str) -> tuple[str, str]:
    return f"{inst}_pins", inst.replace("_", "-")


def render(doc: dict, soc: dict, links: dict,
           ownership: dict[str, str] | None = None) -> tuple[str, set[str]]:
    """(dtsi text, node labels it references)."""
    own = ownership or resolve_ownership(doc)
    linux_dt = soc.get("linux_dt") or {}
    pins: list[str] = []
    nodes: list[str] = []
    labels: set[str] = set()
    for inst, e in sorted(doc["assignable"].items()):
        ld = linux_dt.get(e.get("soc_instance"))
        if ld is None:
            raise GenError(f"assignable.{inst}: soc_instance {e.get('soc_instance')!r} "
                           f"is not a key of {SOC.as_posix()} linux_dt")
        label = ld["label"]
        head = f"/* {inst} ({e['soc_instance']}, &{label}): "
        if e.get("hw_blocked"):
            nodes.append(f"{head}left disabled -- hardware-blocked on every core:\n"
                         f" * {e['hw_blocked']['reason']} */")
            continue
        if own[inst] != "a55":
            nodes.append(f"{head}owned by {own[inst]}; Linux leaves it alone. */")
            continue
        rows = instance_pfc(soc, e)
        gaps = [r["peripheral"] for r, p in rows if p is None]
        if gaps:
            nodes.append(f"{head}GAP -- no PFC function code in {SOC.as_posix()}\n"
                         f" * linux_dt.{e['soc_instance']}.pinmux for {gaps}; node left as the vendor\n"
                         " * dtsi has it (disabled).  Not guessed. */")
            continue
        grp_label, grp_node = _group(inst)
        body = "".join(
            ("\t\tpinmux = " if i == 0 else "\t\t\t ")
            + f"<RZV2N_PORT_PINMUX({port[-1]}, {pin}, {func})>{';' if i == len(rows) - 1 else ','}"
            f" /* {r['pad']} = {r['peripheral']} */\n"
            for i, (r, (port, pin, func)) in enumerate(rows))
        pins.append(f"\t{grp_label}: {grp_node} {{\n{body}\t}};\n")
        labels.add(label)
        chan = ld.get("channel")
        node = (f"{head}owned by a55. */\n&{label} {{\n"
                f"\tpinctrl-0 = <&{grp_label}>;\n\tpinctrl-names = \"default\";\n"
                f"\tstatus = \"okay\";\n")
        if chan is not None:
            node += f"\n\tchannel{chan} {{\n\t\tstatus = \"okay\";\n\t}};\n"
            labels.add(f"{label}/channel{chan}")
        nodes.append(node + "};")
    out = (
        "/*\n"
        f" * GENERATED by scripts/gen_linux_ownership_dt.py from {SRC}\n"
        f" * and {SOC.as_posix()} -- DO NOT EDIT BY HAND.\n"
        " *\n"
        " * Per-product core ownership on the A55 side: each assignable resource\n"
        " * the SoM default gives to Linux is enabled here with pinctrl built from\n"
        " * the metadata; resources owned by the Cortex-M33 or hardware-blocked\n"
        " * stay disabled.  Regenerate after editing either source.\n"
        " */\n\n")
    if pins:
        out += "&pinctrl {\n" + "\n".join(pins) + "};\n\n"
    out += "\n\n".join(nodes) + "\n"
    names = ", ".join(f'"{c}"' for c in cm33_clocks(doc, soc, links, own))
    out += ("\n/*\n * Module clocks the Cortex-M33 uses (enabled supervisor links + CM33-owned\n"
            " * assignable instances): the CPG driver keeps them, and their bus-stop gates,\n"
            " * on through clk_disable_unused (0001-clk-renesas-rzv2h-cpg-cm33-owned-clocks).\n"
            " */\n"
            f"&cpg {{\n\trenesas,cm33-owned-clocks = {names};\n}};\n")
    labels.add("cpg")
    return out, labels


def check_vendor(labels: set[str], vendor_dtsi: Path) -> list[str]:
    text = vendor_dtsi.read_text(encoding="utf-8")
    have = set(re.findall(r"^\s*([a-z][a-z0-9_]*):\s*[\w-]+(?:@[0-9a-f]+)?\s*\{", text, re.M))
    msgs = []
    for lab in sorted(labels):
        base, _, child = lab.partition("/")
        if base not in have:
            msgs.append(f"label &{base} is not defined in {vendor_dtsi.name}")
        elif child and not re.search(rf"^\s*{child}\s*\{{", text, re.M):
            msgs.append(f"node {child} (under &{base}) is not defined in {vendor_dtsi.name}")
    return msgs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", type=Path, default=REPO)
    ap.add_argument("--check", action="store_true", help="exit 1 if the committed output is stale")
    ap.add_argument("--vendor-dtsi", type=Path, help="verify referenced labels exist in this r9a09g056.dtsi")
    args = ap.parse_args()
    try:
        doc, soc, links = _load(args.root)
        text, labels = render(doc, soc, links)
    except GenError as e:
        print(f"gen_linux_ownership_dt: {e}", file=sys.stderr)
        return 1
    if args.vendor_dtsi:
        bad = check_vendor(labels, args.vendor_dtsi)
        for m in bad:
            print(f"gen_linux_ownership_dt: {m}", file=sys.stderr)
        if bad:
            return 1
        print(f"OK   every referenced node exists in {args.vendor_dtsi.name}: {sorted(labels)}")
    out = args.root / OUT
    if args.check:
        if not out.is_file() or out.read_text(encoding="utf-8") != text:
            print(f"gen_linux_ownership_dt: {OUT.as_posix()} is stale -- run "
                  "python3 scripts/gen_linux_ownership_dt.py", file=sys.stderr)
            return 1
        print(f"OK   {OUT.as_posix()} in sync")
        return 0
    if not out.is_file() or out.read_text(encoding="utf-8") != text:
        out.write_text(text, encoding="utf-8", newline="\n")
        print(f"wrote {OUT.as_posix()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
