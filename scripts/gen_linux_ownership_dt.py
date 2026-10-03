#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Generate the Linux devicetree fragment for per-product core ownership on the
RZ/V2N SoM family, so the A55 tree follows the same metadata the CM33 board
tree and the planner follow (#2660).  This writes the committed SoM-DEFAULT
fragment; a project with `ownership:` overrides gets its own via
`alp_project.py --emit linux-ownership-dts` (same renderer,
scripts/alp_orchestrate/linux_ownership.py).

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

  * owned by the A55, not hw_blocked and `linux_enable: true` (which needs
    `linux_evidence`; the default is false, so nothing changes): the node is
    enabled with a pinctrl
    group built from the rows' pads + the SoC linux_dt PFC codes -- but ONLY if
    the SoC carries a function code for every row.  A missing code is a GAP
    comment, never a guessed number, and the node stays as the vendor dtsi has it;
  * hw_blocked: left untouched (disabled) and the reason is quoted;
  * owned by the CM33: explicitly `status = "disabled"` for Linux.

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
    python3 scripts/gen_linux_ownership_dt.py --fragment F --vendor-dtsi PATH
    # per project, straight from the orchestrator's system-manifest.yaml (its
    # resolved `ownership:`), as the linux-renesas bbappend does:
    python3 scripts/gen_linux_ownership_dt.py --manifest M --output F --vendor-dtsi PATH
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

from alp_orchestrate.models import OrchestratorError  # noqa: E402
from alp_orchestrate.linux_ownership import (  # noqa: E402
    SRC, GenError, load_supervisor_links, render)
from alp_orchestrate.ownership import (  # noqa: E402
    load_ownership_doc, ownership_doc_rel, resolve_ownership)

OUT = Path("meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-ownership.dtsi")
SOC = Path("metadata/socs/renesas/rzv2n/n44.json")
LINKS = "metadata/e1m_modules/v2n/supervisor-links.yaml"


def _load(root: Path) -> tuple[dict, dict, dict]:
    doc = load_ownership_doc(root / "metadata", "v2n")
    if not doc or not doc.get("assignable"):
        raise GenError(f"{SRC} has no assignable: block")
    soc = json.loads((root / SOC).read_text(encoding="utf-8"))
    links = yaml.safe_load((root / LINKS).read_text(encoding="utf-8"))["supervisor_links"]
    return doc, soc, links


def render_manifest(root: Path, manifest: Path) -> tuple[str, set[str]]:
    """The fragment for the project a system-manifest.yaml describes: its
    resolved `ownership:` (validated again by resolve_ownership), the family's
    core-ownership.yaml and the SoC JSON the manifest's `hw_info.silicon` names."""
    from alp_orchestrate.loader import _sku_family_dir
    m = yaml.safe_load(manifest.read_text(encoding="utf-8"))
    hw = m.get("hw_info") or {}
    own = m.get("ownership")
    if own is None:
        raise GenError(f"{manifest}: no `ownership:` -- the SoM declares no assignable resources "
                       "or the manifest predates it; re-run `alp_project.py --emit system-manifest`")
    meta, fam = root / "metadata", _sku_family_dir(hw.get("sku", ""))
    doc = load_ownership_doc(meta, fam)
    if not doc or not doc.get("assignable"):
        raise GenError(f"{manifest}: SoM {hw.get('sku')!r} has no assignable: core ownership")
    if set(own) != set(doc["assignable"]):
        raise GenError(f"{manifest}: ownership instances {sorted(own)} != metadata "
                       f"{sorted(doc['assignable'])}; stale manifest, re-run the emit")
    from alp_project_loader import resolve_soc_path
    soc_path = resolve_soc_path(hw.get("silicon"), meta)
    if soc_path is None:
        raise GenError(f"{manifest}: hw_info.silicon {hw.get('silicon')!r} is not a vendor:family:part key")
    soc = json.loads(soc_path.read_text(encoding="utf-8"))
    return render(doc, soc, load_supervisor_links(meta, fam), resolve_ownership(doc, own),
                  src=ownership_doc_rel(meta, fam))


def fragment_labels(text: str) -> set[str]:
    """Node labels a fragment references: `&label {` plus its `channelN` children
    (as `label/channelN`, the form check_vendor verifies)."""
    labels, cur = set(), None
    for line in text.splitlines():
        m = re.match(r"^&([a-z][a-z0-9_]*)\s*\{", line)
        if m:
            cur = m.group(1)
            labels.add(cur)
        elif cur and (c := re.match(r"^	(channel\d+)\s*\{", line)):
            labels.add(f"{cur}/{c.group(1)}")
    return labels


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
    ap.add_argument("--fragment", type=Path,
                    help="verify a per-project fragment (alp_project.py --emit linux-ownership-dts) "
                         "against --vendor-dtsi instead of writing/checking the SoM default")
    ap.add_argument("--manifest", type=Path,
                    help="render from this system-manifest.yaml's resolved ownership (per project) "
                         "instead of the SoM default; needs --output")
    ap.add_argument("--output", type=Path, help="with --manifest: write the fragment here")
    ap.add_argument("--vendor-dtsi", type=Path, help="verify referenced labels exist in this r9a09g056.dtsi")
    args = ap.parse_args()
    if args.fragment:
        if not args.vendor_dtsi:
            print("gen_linux_ownership_dt: --fragment needs --vendor-dtsi", file=sys.stderr)
            return 1
        labels = fragment_labels(args.fragment.read_text(encoding="utf-8"))
        bad = check_vendor(labels, args.vendor_dtsi)
        for m in bad:
            print(f"gen_linux_ownership_dt: {m}", file=sys.stderr)
        if not bad:
            print(f"OK   {args.fragment.name}: every node exists in {args.vendor_dtsi.name}: {sorted(labels)}")
        return 1 if bad else 0
    try:
        if args.manifest:
            text, labels = render_manifest(args.root, args.manifest)
        else:
            doc, soc, links = _load(args.root)
            text, labels = render(doc, soc, links)
    except (GenError, OrchestratorError) as e:
        print(f"gen_linux_ownership_dt: {e}", file=sys.stderr)
        return 1
    if args.vendor_dtsi:
        bad = check_vendor(labels, args.vendor_dtsi)
        for m in bad:
            print(f"gen_linux_ownership_dt: {m}", file=sys.stderr)
        if bad:
            return 1
        print(f"OK   every referenced node exists in {args.vendor_dtsi.name}: {sorted(labels)}")
    if args.manifest:
        if not args.output:
            print("gen_linux_ownership_dt: --manifest needs --output", file=sys.stderr)
            return 1
        args.output.write_text(text, encoding="utf-8", newline="\n")
        print(f"wrote {args.output}")
        return 0
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
