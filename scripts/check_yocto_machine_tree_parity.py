#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: every `topology.<core>.machine:` Yocto MACHINE declared in a
`metadata/e1m_modules/E1M-*.yaml` SoM preset must either ship a real
conf under `meta-alp-sdk/conf/machine/` or be explicitly named, with a
reason, in `alp_orchestrate.orchestrator.YOCTO_MACHINE_UNBUILDABLE`.

Why this exists (issue #1982 follow-up): a review of that fix found the
planner's `YOCTO_MACHINE_UNBUILDABLE` denylist named only the two AEN
A32 MACHINEs whose conf happens to exist (`e1m-aen801-a32`,
`e1m-aen701-a32`) -- three more, `e1m-aen501-a32` / `e1m-aen601-a32` /
`e1m-aen803-a32`, declare a `topology.a32_cluster.machine:` too and ship
NO conf under `meta-alp-sdk/conf/machine/` at all, which is strictly
MORE unbuildable (BitBake fails to find the MACHINE before it can parse
a single `require`) -- yet nothing caught the planner still handing out
`bitbake alp-image-edge` for them. This gate is modelled on
`check_board_target_tree_parity.py` (issue #999) but is a ONE-WAY
absence check, not that gate's full self-cleaning shape, because a
Yocto MACHINE conf existing is weaker evidence than a Zephyr board
tree existing: a real conf file is not proven buildable by that fact
alone (both `e1m-aen801-a32` and `e1m-aen701-a32` ship a conf and are
still unbuildable for reasons this gate cannot evaluate: an upstream
`require` target missing entirely, or the target layer's
LAYERSERIES_COMPAT). Deciding THAT still requires the hand-reviewed
prose in `YOCTO_MACHINE_UNBUILDABLE` -- this gate only enforces that
every gap is *declared* somewhere, the same division of labour
`check_board_target_tree_parity.py` already draws between "the tree"
and its own `_NOT_YET_SUPPORTED`.

What this means in practice, and where this gate deliberately does
NOT fully mirror `check_board_target_tree_parity.py`:

  * a `machine:` with NO conf file and NO `YOCTO_MACHINE_UNBUILDABLE`
    entry -- FAILS: either ship the conf, or declare the gap with why.
  * a `machine:` with NO conf file that IS declared -- passes (the
    declared-gap state issue #1982 leaves E1M-AEN501/601/803 in).
  * a `machine:` that HAS a conf file, declared or not -- this gate
    does NOT flag it either way. A Zephyr `board.yml`'s `name:` field
    resolving is a strong existence proof (Zephyr's own board-discovery
    would find and try to build it); a BitBake `.conf` file merely
    existing on disk is not -- `e1m-aen801-a32.conf` and
    `e1m-aen701-a32.conf` both exist and are BOTH still unbuildable
    (a `require` naming a file absent upstream, or a commented-out
    `require` wiring nothing at all), so "the conf now exists" cannot
    drive an automatic revocation here the way it does for board trees.
    Removing an entry once its MACHINE genuinely resolves stays a
    human call, recorded in `YOCTO_MACHINE_UNBUILDABLE`'s own comment.

The self-cleaning this gate DOES provide: the "no conf shipped at all"
failure class (today: E1M-AEN501/601/803) cannot silently regress back
to undeclared, and no SKU can gain a `machine:` target that falls
through both checks unnoticed -- one list, consulted by both the
planner and this gate, covers every AEN A32 SKU today and any future
one.

Run locally:

    python3 scripts/check_yocto_machine_tree_parity.py

CI wires this in `pr-metadata-validate.yml`.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent


def _load_unbuildable_machines() -> dict[str, str]:
    """Import the planner's own denylist rather than re-declaring it --
    this gate and `_slice_command` must consult the SAME dict, or the
    two drift the way the hand-maintained list this gate replaces did."""
    sys.path.insert(0, str(REPO / "scripts"))
    from alp_orchestrate.orchestrator import YOCTO_MACHINE_UNBUILDABLE
    return YOCTO_MACHINE_UNBUILDABLE


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    presets_dir = root / "metadata" / "e1m_modules"
    machine_dir = root / "meta-alp-sdk" / "conf" / "machine"
    if not presets_dir.is_dir():
        return problems
    unbuildable = _load_unbuildable_machines()
    real_confs = {p.stem for p in machine_dir.glob("*.conf")} if machine_dir.is_dir() else set()

    for preset in sorted(presets_dir.glob("E1M-*.yaml")):
        with preset.open(encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
        sku = doc.get("sku") or preset.stem
        topology = doc.get("topology") or {}
        if not isinstance(topology, dict):
            continue
        for core, entry in topology.items():
            if not isinstance(entry, dict) or "machine" not in entry:
                continue
            machine = str(entry["machine"])
            has_conf = machine in real_confs
            is_declared = machine in unbuildable
            if not has_conf and not is_declared:
                problems.append(
                    f"{preset.name}: {sku}/{core} declares MACHINE "
                    f"'{machine}' but meta-alp-sdk/conf/machine/{machine}.conf "
                    "does not exist -- ship the conf, or add it to "
                    "YOCTO_MACHINE_UNBUILDABLE in "
                    "scripts/alp_orchestrate/orchestrator.py with why"
                )

    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    problems = find_problems(args.root)
    if problems:
        print("check_yocto_machine_tree_parity: found problems:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    print("OK: every yocto machine: target resolves to a real conf or a "
          "declared YOCTO_MACHINE_UNBUILDABLE entry.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
