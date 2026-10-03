#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Cross-core pad-claim gate for the AMP RZ/V2N SoM (issue #1142).

`metadata/pinmux/v2n.yaml` can now say WHICH core drives a pad
(`core: "a55"` / `"m33"`, issue #1157), but nothing read that field.
Meanwhile the A55's own pad claims live in a completely different tree --
the Linux devicetree under `meta-alp-sdk/recipes-kernel/linux/` -- and
neither side can see the other's claim at build time.  That blind spot is
not hypothetical: `docs/errata-e1m-x-v2n.md` records, as errata E3, the USB
over-current gpio-hog on `P9.6` silently clobbering the CM33's live
`SCK7` mux at every Linux boot after the GD32 supervisor SPI took the pad
on 2026-06-03, because the hog's `PMC9`/`PM9` byte-RMW lands at ~1.9 s,
inside the CM33's pin-setup window.  A bench session found that; nothing
in CI could.

This gate checks both directions:

  * a Linux DT pad claim on a pad resolved to the CM33 is an error;
  * a CM33 board claim (an ENABLED node, in zephyr/boards/alp/e1m_v2*_m33_sm,
    referencing a pinctrl group) on a pad resolved to the A55 is an error.

"Resolved" = the fixed `core:` rows of metadata/pinmux/v2n.yaml plus every
`assignable:` instance of core-ownership.yaml at its SoM DEFAULT core
(a board.yaml `ownership:` override is per project: `--project board.yaml`
re-runs both passes with the project's resolved ownership and its generated
Linux fragment in place of the committed default).
A `status = "disabled"` CM33 node (an assignable peripheral a project may
enable) is not a claim; Linux claims stay textual, see below.

WHAT COUNTS AS A LINUX PAD CLAIM.  Both port-pad macros the RZ/V2N
bindings expose, wherever they appear in a `.dts`/`.dtsi` under
`meta-alp-sdk/recipes-kernel/linux/`:

  * `RZV2N_GPIO(<port>, <pin>)`        -- gpio-hogs and gpio consumers
  * `RZV2N_PORT_PINMUX(<port>, <pin>, <func>)` -- pinctrl groups

Both resolve to pad `P<port><pin>` (`RZV2N_GPIO(A, 0)` -> `PA0`,
`RZV2N_PORT_PINMUX(0, 6, 1)` -> `P06`), which is the spelling
`metadata/pinmux/v2n.yaml` uses in `silicon_pad`.  Dedicated-ball groups
that name pins as strings (`pins = "SD0CLK"`) are not port pads and are
out of scope.

ponytail: textual claim, not node-status aware -- a claim inside a
`status = "disabled"` node, or in a pinctrl group nothing references, is
still reported.  That is deliberate and conservative: a CM33-owned pad
named in a Linux DT is worth a human look either way, and node-status
resolution needs a real dtc pass this gate is not worth spending.

Exit codes:
* 0  -- no Linux DT claim on a CM33-attributed pad.
* 1  -- one or more claims, or a stale exemption.

Run locally:

    python3 scripts/check_amp_pad_claims.py

CI wires this in `pr-metadata-validate.yml`.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent

PINMUX = Path("metadata") / "pinmux" / "v2n.yaml"
LINUX_DT_DIR = Path("meta-alp-sdk") / "recipes-kernel" / "linux"

# (peripheral, pad) pairs the metadata attributes to the CM33 that the
# Linux DT is nevertheless ALLOWED to claim.  Every entry needs both
# claims cited, and an entry matching no `core: "m33"` row is a hard
# error below -- an exemption that quietly stops applying is how a gate
# rots into decoration.
#
# BRD_I2C / RIIC8 is not dual-master: Cortex-A55/Linux is the SOLE
# master of the whole RIIC8 bus (metadata/e1m_modules/v2n/
# core-ownership.yaml attributes P06/P07 to `core: "a55"`, not "m33");
# on the CM33 board `&i2c8` is disabled and has no `alp-i2c0` alias
# (scripts/gen_zephyr_board.py `_v2n_dts()`), so its unreferenced
# `i2c8_pins` group never muxes these pads.  No EXEMPT entry is
# needed: this gate only flags a Linux DT claim on a `core: "m33"` pad,
# and P06/P07 do not resolve to one.
EXEMPT: dict[tuple[str, str], str] = {}

_GPIO_RE = re.compile(r"RZV2N_GPIO\(\s*([0-9A-Z])\s*,\s*(\d+)\s*\)")
_PINMUX_RE = re.compile(r"RZV2N_PORT_PINMUX\(\s*([0-9A-Z])\s*,\s*(\d+)\s*,\s*\d+\s*\)")


CM33_BOARDS = Path("zephyr") / "boards" / "alp"
_PINCTRL_GROUP_RE = re.compile(r"^\t(\w+):\s*[\w-]+\s*\{", re.M)
_RZV_PINMUX_RE = re.compile(r"RZV_PINMUX\(\s*PORT_0([0-9A-Z])\s*,\s*(\d+)\s*,")


def _pads_by_core(root: Path, core: str,
                  ownership: "dict[str, str] | None" = None) -> dict[str, list[str]]:
    """pad -> peripherals, for every pad resolved to `core`: the pinmux
    table's fixed `core:` rows plus the assignable owners (SoM defaults, or a
    project's resolved `ownership` when given).

    Keyed pad -> LIST, because `(peripheral, pad)` is the table's real key:
    one pad can carry more than one row, and the two ends of an inter-chip
    link share a peripheral name across different pads.
    """
    doc = yaml.safe_load((root / PINMUX).read_text(encoding="utf-8"))
    out: dict[str, list[str]] = {}
    for pad in doc["pads"]:
        if pad.get("core") == core:
            out.setdefault(pad["silicon_pad"], []).append(
                pad["silicon_peripheral"])
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from alp_orchestrate.ownership import load_ownership_doc, resolve_ownership
    own = load_ownership_doc(root / "metadata", "v2n")
    if own:
        for inst, owner in (ownership or resolve_ownership(own)).items():
            if owner == core:
                for r in own["assignable"][inst]["rows"]:
                    out.setdefault(r["pad"], []).append(r["peripheral"])
    return out


def _cm33_claims(root: Path) -> list[tuple[str, int, str]]:
    """(file, line, pad) for every pad an ENABLED node of a CM33 V2N/V2M
    board dts claims through `pinctrl-0` (the groups come from the board's
    own -pinctrl.dtsi).  Disabled nodes claim nothing."""
    out: list[tuple[str, int, str]] = []
    for pin in sorted((root / CM33_BOARDS).glob("e1m_v2*_m33_sm/*-pinctrl.dtsi")):
        text = pin.read_text(encoding="utf-8")
        marks = list(_PINCTRL_GROUP_RE.finditer(text))
        groups = {
            m.group(1): [f"P{a}{b}" for a, b in _RZV_PINMUX_RE.findall(
                text[m.end():marks[i + 1].start() if i + 1 < len(marks) else len(text)])]
            for i, m in enumerate(marks)}
        for dts in sorted(pin.parent.glob("*.dts")):
            node = None
            for n, line in enumerate(dts.read_text(encoding="utf-8").splitlines(), 1):
                m = re.match(r"^&(\w+) \{$", line)
                if m:
                    node = {"refs": [], "line": n, "status": None}
                elif node is not None and line == "};":
                    if node["status"] == "okay":
                        for label in node["refs"]:
                            out += [(dts.relative_to(root).as_posix(), node["line"], pad)
                                    for pad in groups.get(label, ())]
                    node = None
                elif node is not None:
                    m = re.match(r'^\tpinctrl-0 = <(.*)>;$', line)
                    if m:
                        node["refs"] = re.findall(r"&(\w+)", m.group(1))
                    m = re.match(r'^\tstatus = "(\w+)";$', line)
                    if m:
                        node["status"] = m.group(1)
    return out


# The committed SoM-default fragment; a project replaces it with its own.
DEFAULT_FRAGMENT = "e1m-v2n-ownership.dtsi"


def find_project_problems(project) -> list[str]:
    """The Linux-DT-vs-CM33 pass (and the reverse) over the SoM dtsi + carrier
    dtsi + the project's generated fragment (`--emit linux-ownership-dts`), with
    the project's RESOLVED ownership (a board.yaml override included)."""
    from alp_orchestrate.linux_ownership import emit_linux_ownership_dts
    return find_problems(project.effective_metadata_root().parent, project.ownership,
                         emit_linux_ownership_dts(project))


def find_problems(root: Path, ownership: "dict[str, str] | None" = None,
                  project_fragment: "str | None" = None) -> list[str]:
    """`ownership` / `project_fragment` (None = SoM default): the project's
    resolved ownership, and its generated Linux fragment, which stands in for
    the committed default fragment."""
    problems: list[str] = []
    pinmux = root / PINMUX
    if not pinmux.is_file():
        return [f"{PINMUX.as_posix()}: missing -- this gate cannot run without it"]
    m33 = _pads_by_core(root, "m33", ownership)

    stale = [key for key in EXEMPT if key[0] not in m33.get(key[1], ())]
    for peripheral, pad in stale:
        problems.append(
            f"EXEMPT entry ({peripheral!r}, {pad!r}) matches no "
            f"`core: \"m33\"` row in {PINMUX.as_posix()} -- the attribution "
            f"it excuses is gone, so drop the entry from "
            f"scripts/check_amp_pad_claims.py"
        )

    dt_dir = root / LINUX_DT_DIR
    sources = [(path.relative_to(root).as_posix(), path.read_text(encoding="utf-8"))
               for path in sorted(dt_dir.rglob("*.dts*"))
               if path.suffix in (".dts", ".dtsi")
               and not (project_fragment is not None and path.name == DEFAULT_FRAGMENT)]
    if project_fragment is not None:
        sources.append(("project Linux fragment", project_fragment))
    for rel, text in sources:
        for lineno, line in enumerate(text.splitlines(), start=1):
            for match in (*_GPIO_RE.finditer(line), *_PINMUX_RE.finditer(line)):
                pad = f"P{match.group(1)}{match.group(2)}"
                for peripheral in m33.get(pad, ()):
                    if (peripheral, pad) in EXEMPT:
                        continue
                    problems.append(
                        f"{rel}:{lineno}: Linux devicetree claims {pad} "
                        f"({match.group(0)}), which {PINMUX.as_posix()} "
                        f"attributes to the CM33 (core: \"m33\", "
                        f"silicon_peripheral: {peripheral!r}).  A Linux port "
                        f"claim is a non-atomic PMC/PM byte-RMW at pinctrl "
                        f"probe (~1.9 s) that can clobber the CM33's live mux "
                        f"-- the errata E3 P9.6/SCK7 regression "
                        f"(errata E3, docs/errata-e1m-x-v2n.md).  Drop the claim, or "
                        f"correct the attribution in "
                        f"metadata/e1m_modules/v2n/core-ownership.yaml with "
                        f"evidence"
                    )
    a55 = _pads_by_core(root, "a55", ownership)
    for rel, lineno, pad in _cm33_claims(root):
        for peripheral in a55.get(pad, ()):
            problems.append(
                f"{rel}:{lineno}: the CM33 board enables a node whose pinctrl "
                f"claims {pad}, which resolves to the A55 "
                f"(silicon_peripheral: {peripheral!r}; {PINMUX.as_posix()} "
                f"`core: \"a55\"` or an `assignable:` default in "
                f"metadata/e1m_modules/v2n/core-ownership.yaml).  Leave the node "
                f"`disabled` on the board (a project enables it via "
                f"board.yaml `ownership:`) or correct the attribution with "
                f"evidence"
            )
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT,
                        help="repository root to check (default: this repo)")
    parser.add_argument("--project", type=Path, action="append", default=[],
                        help="also run the full pass with this board.yaml's resolved "
                             "ownership and generated Linux fragment; repeatable")
    args = parser.parse_args()

    problems = find_problems(args.root)
    if args.project:
        from alp_orchestrate import load_board_yaml
        for b in args.project:
            problems += [f"{b}: {m}" for m in find_project_problems(
                load_board_yaml(b, metadata_root=args.root / "metadata"))]
    if problems:
        for p in problems:
            print(f"amp-pad-claims: {p}", file=sys.stderr)
        return 1
    print("OK: no cross-core pad claim (Linux DT vs CM33 pads, CM33 board vs A55 pads).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
