#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: every `topology.<core>.board:` Zephyr target declared in a
`metadata/e1m_modules/E1M-*.yaml` SoM preset must resolve to a real
board tree under `zephyr/boards/alp/`, UNLESS the `(sku, core, board)`
triple is explicitly named in `_NOT_YET_SUPPORTED` below.

Why this exists (issue #999): a full sweep of `origin/dev` @ 8879597d
found 11 of 17 declared `topology.<core>.board:` targets -- 8 of 12
across the AEN family, plus `E1M-NX9101` (m33), `E1M-V2M102`
(m33_sm), and `E1M-V2N102` (m33_sm) -- naming a board that has no
tree, e.g. `E1M-AEN701`'s `m55_he` names `alp_e1m_aen701_m55_he`,
which `zephyr/boards/alp/` does not ship.
Nothing caught it: alp-sdk's own planner (`--emit build-plan`) emits
the unbuildable `west build -b alp_e1m_aen701_m55_he` command with
rc=0, and a customer who hand-runs `west build` on that SKU hits the
identical "No board named ... Invalid BOARD" CMake error alp-sdk's
own tooling would have hit first.

Declaring a SKU ahead of its board bring-up is a legitimate roadmap
state (AEN801 is the lead part; 301/501/701 are deprioritised;
NX9101 is a placeholder MPN; V2M102/V2N102 are larger-memory variants
of a shipping PCB awaiting their own board tree) -- so this gate does
NOT require deleting the `board:` key or shipping a stub tree. It
requires the gap be *declared*: every missing target must be named,
with a reason, in `_NOT_YET_SUPPORTED`.

What belongs in `_NOT_YET_SUPPORTED`: a `(sku, core, board)` triple
for a target that is real roadmap (silicon exists, board bring-up is
simply not done yet), each with a one-line reason a reviewer can
check against the SKU's own `status:` block or the roadmap. What does
NOT belong: a target that's missing because of a typo, a renamed
board, or silicon that was dropped from the roadmap entirely -- those
get the `board:` key removed instead of allowlisted. If an entry's
tree later ships, this gate itself flags the entry as stale (the
board now resolves) so the allowlist self-cleans instead of growing
forever.

Run locally:

    python3 scripts/check_board_target_tree_parity.py

CI wires this in `pr-metadata-validate.yml`.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent

# (sku, core, bare board target) triples that are known-missing a
# zephyr/boards/alp/ tree today, on purpose. Each entry names the
# reason so the gap reads as a decision, not an oversight -- see the
# module docstring for what belongs here. Filed as issue #999.
_NOT_YET_SUPPORTED: frozenset[tuple[str, str, str]] = frozenset(
    {
        # E1M-AEN301 (Alif Ensemble E3): AEN801 is the lead part; the
        # smaller E3/E4/E5/E7 AEN SKUs are deprioritised behind it and
        # have no board bring-up yet.
        ("E1M-AEN301", "m55_hp", "alp_e1m_aen301_m55_hp"),
        ("E1M-AEN301", "m55_he", "alp_e1m_aen301_m55_he"),
        # E1M-AEN401 (Alif Ensemble E4): the M55-HP core shipped a
        # tree (alp_e1m_aen401_m55_hp); the M55-HE core has not.
        ("E1M-AEN401", "m55_he", "alp_e1m_aen401_m55_he"),
        # E1M-AEN501 (Alif Ensemble E5): status.preliminary=true --
        # the E5 datasheet is not yet public, so no board bring-up.
        ("E1M-AEN501", "m55_hp", "alp_e1m_aen501_m55_hp"),
        ("E1M-AEN501", "m55_he", "alp_e1m_aen501_m55_he"),
        # E1M-AEN601 (Alif Ensemble E6): the M55-HP core shipped a
        # tree (alp_e1m_aen601_m55_hp); the M55-HE core has not.
        ("E1M-AEN601", "m55_he", "alp_e1m_aen601_m55_he"),
        # E1M-AEN701 (Alif Ensemble E7): deprioritised behind the
        # AEN801 lead part; no board bring-up yet.
        ("E1M-AEN701", "m55_hp", "alp_e1m_aen701_m55_hp"),
        ("E1M-AEN701", "m55_he", "alp_e1m_aen701_m55_he"),
        # E1M-NX9101 (NXP i.MX 93): status.preliminary=true -- the
        # SKU string itself is a placeholder MPN pending a real BOM.
        ("E1M-NX9101", "m33", "alp_e1m_nx9101_m33"),
        # E1M-V2M102 / E1M-V2N102: larger-memory BOM variants of the
        # same V2N-family PCB as the shipped V2M101/V2N101 boards
        # (see the V2N-family-is-one-PCB project note); the 101
        # variant's tree exists, the 102 variant's board bring-up has
        # not happened yet.
        ("E1M-V2M102", "m33_sm", "alp_e1m_v2m102_m33_sm"),
        ("E1M-V2N102", "m33_sm", "alp_e1m_v2n102_m33_sm"),
        # E1M-AEN803: dual-external-memory BOM variant of the same
        # E1M-AEN-2626 PCB as the shipped E1M-AEN801 boards (both OSPI0
        # memories fitted, vs AEN801's NOR-optional population); AEN801's
        # tree exists, AEN803's own board bring-up has not happened yet.
        ("E1M-AEN803", "m55_hp", "alp_e1m_aen803_m55_hp"),
        ("E1M-AEN803", "m55_he", "alp_e1m_aen803_m55_he"),
    }
)


def _load_real_board_names(boards_dir: Path) -> set[str]:
    """Every real Zephyr board target name shipped in-tree, read from
    each zephyr/boards/alp/<dir>/board.yml's `board: name:` field --
    board EXISTENCE, not naming shape, is what makes a target real."""
    names: set[str] = set()
    if not boards_dir.is_dir():
        return names
    for board_yml in sorted(boards_dir.glob("*/board.yml")):
        with board_yml.open(encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
        name = (doc.get("board") or {}).get("name")
        if isinstance(name, str):
            names.add(name)
    return names


def _expected_tree_dir(bare_board: str) -> str:
    """The zephyr/boards/alp/<dir> name a bare board target would
    live under, per the convention every shipped tree follows: the
    vendor prefix ('alp_') is the parent directory, so it's stripped
    from the leaf dir name (alp_e1m_aen801_m55_he -> e1m_aen801_m55_he)."""
    return bare_board[len("alp_"):] if bare_board.startswith("alp_") else bare_board


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    presets_dir = root / "metadata" / "e1m_modules"
    boards_dir = root / "zephyr" / "boards" / "alp"
    if not presets_dir.is_dir():
        return problems
    real_boards = _load_real_board_names(boards_dir)

    for preset in sorted(presets_dir.glob("E1M-*.yaml")):
        with preset.open(encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
        sku = doc.get("sku") or preset.stem
        topology = doc.get("topology") or {}
        if not isinstance(topology, dict):
            continue
        for core, entry in topology.items():
            if not isinstance(entry, dict) or "board" not in entry:
                continue
            raw = str(entry["board"])
            bare = raw.split()[0].split("/")[0]
            key = (sku, core, bare)
            if bare in real_boards:
                if key in _NOT_YET_SUPPORTED:
                    problems.append(
                        f"{preset.name}: {sku}/{core} board '{bare}' now has "
                        f"a real tree (zephyr/boards/alp/{_expected_tree_dir(bare)}/) "
                        "-- remove its stale entry from _NOT_YET_SUPPORTED in "
                        f"{Path(__file__).name}"
                    )
                continue
            if key in _NOT_YET_SUPPORTED:
                continue
            problems.append(
                f"{preset.name}: {sku}/{core} declares board '{bare}' but "
                f"zephyr/boards/alp/{_expected_tree_dir(bare)}/ does not "
                "exist -- ship the board tree, or add "
                f"('{sku}', '{core}', '{bare}') to _NOT_YET_SUPPORTED in "
                f"{Path(__file__).name} with why"
            )

    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    problems = find_problems(args.root)
    if problems:
        print("check_board_target_tree_parity: found problems:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    print("OK: every board: target resolves to a real tree or a declared "
          "not-yet-supported entry.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
