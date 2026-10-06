# SPDX-License-Identifier: Apache-2.0
"""Linux devicetree fragment for per-product core ownership (RZ/V2N family).

ONE renderer for two callers: `scripts/gen_linux_ownership_dt.py` (the
committed SoM-default fragment) and `--emit linux-ownership-dts` (the
per-project fragment from the project's resolved `ownership:`).  Inputs are
read, never restated: the family core-ownership.yaml, its
supervisor-links.yaml and the SoC JSON `linux_dt` block (`project.soc_spec`).
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Optional

import yaml

from .models import OrchestratorError
from .ownership import instance_pfc, load_ownership_doc, ownership_doc_rel, resolve_ownership

# The V2N SoM-default source; a project passes its own family's via render(src=).
SRC = "metadata/e1m_modules/v2n/core-ownership.yaml"


class GenError(OrchestratorError):
    pass


def load_supervisor_links(metadata_root: Path, family_dir: Optional[str]) -> dict:
    """The family's enabled/disabled supervisor links (V2M shares V2N's)."""
    for fam in (family_dir, "v2n" if (family_dir or "").startswith("v2n") else None):
        if fam:
            p = metadata_root / "e1m_modules" / fam / "supervisor-links.yaml"
            if p.is_file():
                return yaml.safe_load(p.read_text(encoding="utf-8"))["supervisor_links"]
    return {}


def emit_linux_ownership_dts(project) -> str:
    """The Linux ownership fragment for this project's resolved ownership,
    or a comment-only stub when the SoM family has no `assignable:` block."""
    from .loader import _sku_family_dir
    root, fam = project.effective_metadata_root(), _sku_family_dir(project.sku)
    doc = load_ownership_doc(root, fam)
    if not doc or not doc.get("assignable"):
        return "/* No assignable core ownership for this SoM family; nothing to emit. */\n"
    return render(doc, project.soc_spec, load_supervisor_links(root, fam), project.ownership,
                  src=ownership_doc_rel(root, fam))[0]


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
                           f"soc linux_dt")
        add(ent["cpg_clocks"])
    for inst, e in sorted(doc["assignable"].items()):
        if own[inst] == "m33" and not e.get("hw_blocked"):
            ent = linux_dt.get(e.get("soc_instance")) or {}
            if not ent.get("cpg_clocks"):
                raise GenError(f"assignable.{inst}: owned by m33 but linux_dt.{e.get('soc_instance')} "
                               "has no cpg_clocks")
            add(ent["cpg_clocks"])
    return clocks


def can_netdev_map(doc: dict, soc: dict, own: dict[str, str]) -> list[str]:
    """Linux netdev per E1M CAN bus id (index = N of `e1m_canN`), "" = none.

    rcar_canfd names netdevs `can0..` in probe order of the enabled channels
    (ascending channel), and exposes no channel id at runtime (#2352), so the
    name is the rank of the instance's SoC channel among the channels Linux
    drives here: a55-owned, not hw_blocked (m33/hw_blocked ones are disabled
    or untouched).  Assumes the vendor dtsi enables no other channel."""
    linux_dt = soc.get("linux_dt") or {}
    cans: dict[int, int] = {}
    for inst, e in doc["assignable"].items():
        m = re.fullmatch(r"e1m_can(\d+)", inst)
        chan = (linux_dt.get(e.get("soc_instance")) or {}).get("channel")
        if m and chan is not None and own[inst] == "a55" and not e.get("hw_blocked"):
            cans[int(m.group(1))] = chan
    if not cans:
        return []
    ranked = sorted(cans.values())
    return [f"can{ranked.index(cans[b])}" if b in cans else "" for b in range(max(cans) + 1)]


def _group(inst: str) -> tuple[str, str]:
    return f"{inst}_pins", inst.replace("_", "-")


def render(doc: dict, soc: dict, links: dict,
           ownership: Optional[dict[str, str]] = None,
           src: str = SRC) -> tuple[str, set[str]]:
    """(dtsi text, node labels it references).  `src` = the family's
    core-ownership.yaml path, quoted in the output."""
    own = ownership or resolve_ownership(doc)
    linux_dt = soc.get("linux_dt") or {}
    pins: list[str] = []
    nodes: list[str] = []
    labels: set[str] = set()
    for inst, e in sorted(doc["assignable"].items()):
        ld = linux_dt.get(e.get("soc_instance"))
        if ld is None:
            raise GenError(f"assignable.{inst}: soc_instance {e.get('soc_instance')!r} "
                           f"is not a key of soc linux_dt")
        label = ld["label"]
        head = f"/* {inst} ({e['soc_instance']}, &{label}): "
        if e.get("hw_blocked"):
            nodes.append(f"{head}left disabled -- hardware-blocked on every core:\n"
                         f" * {e['hw_blocked']['reason']} */")
            continue
        if own[inst] != "a55":
            labels.add(label)
            nodes.append(f"{head}owned by {own[inst]}; Linux must not claim it. */\n"
                         f"&{label} {{\n\tstatus = \"disabled\";\n}};")
            continue
        if e.get("linux_enable") and not e.get("linux_evidence"):
            raise GenError(f"assignable.{inst}: linux_enable needs linux_evidence")
        if not e.get("linux_enable"):
            nodes.append(f"{head}owned by a55; left at the vendor status (no `linux_enable` in\n"
                         f" * {src} -- Linux enablement is not bench-evidenced). */")
            continue
        rows = instance_pfc(soc, e)
        gaps = [r["peripheral"] for r, p in rows if p is None]
        if gaps:
            nodes.append(f"{head}GAP -- no PFC function code in the SoC JSON\n"
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
        " * GENERATED (scripts/gen_linux_ownership_dt.py, or `--emit linux-ownership-dts`\n"
        f" * for a project) from {src}\n"
        " * and the SoC JSON linux_dt block -- DO NOT EDIT BY HAND.\n"
        " *\n"
        " * Per-product core ownership on the A55 side: each assignable resource\n"
        " * the SoM default gives to Linux is enabled here with pinctrl built from\n"
        " * the metadata; resources owned by the Cortex-M33 or hardware-blocked\n"
        " * are explicitly disabled.  Regenerate after editing either source.\n"
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
    nd = can_netdev_map(doc, soc, own)
    if nd:
        names = ", ".join(f'"{n}"' for n in nd)
        out += ("\n/*\n * Linux netdev per E1M CAN bus id (index = N of E1M_X_CANN): rcar_canfd numbers\n"
                " * netdevs by probe order, not by channel, so the SDK's SocketCAN backend reads\n"
                " * this instead of assuming can<N> (#2352).\n */\n"
                f"/ {{\n\talp,e1m-can-netdev = {names};\n}};\n")
    return out, labels
