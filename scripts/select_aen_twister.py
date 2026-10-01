#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pick the pr-twister-aen.yml matrix legs a pull request needs (#2541).

Per SKU, a changed path decides:

  * neutral        changelog.d/, docs/, any *.md, tests/scripts/ -- builds
                   nothing, so it never pulls a SKU in;
  * SKU-exclusive  zephyr/boards/alp/e1m_<sku>_m55_{he,hp}/ -- only that
                   SKU's legs build it;
  * anything else  shared input (examples, drivers, west.yml, the workflow
                   itself, ...) -- every SKU runs.

A SKU runs iff some changed path is neither neutral nor exclusive to the
other SKU.  Anything uncertain (no base, failed git call) runs every SKU; the
workflow always runs full on push, schedule and dispatch.

Prints the matrix as a JSON list for `strategy.matrix.include`: one entry per
SKU x twister `--subset i/2` shard.  An empty list means "run nothing".
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

SKUS = {
    "aen801": ("AEN801 (M55-HE + M55-HP)", "e1m_aen801_m55_"),
    "aen803": ("AEN803 (M55-HE + M55-HP)", "e1m_aen803_m55_"),
}
CPU = {"he": "rtss_he", "hp": "rtss_hp"}
NEUTRAL_PREFIXES = ("changelog.d/", "docs/", "tests/scripts/")


def _exclusive_to(path: str) -> str | None:
    for sku, (_, prefix) in SKUS.items():
        if path.startswith(f"zephyr/boards/alp/{prefix}"):
            return sku
    return None


def affected_skus(paths: list[str]) -> list[str]:
    if not paths:
        return list(SKUS)
    out = []
    for sku in SKUS:
        for p in paths:
            if p.endswith(".md") or p.startswith(NEUTRAL_PREFIXES):
                continue
            if _exclusive_to(p) not in (None, sku):
                continue
            out.append(sku)
            break
    return out


def matrix(skus: list[str]) -> list[dict]:
    legs = []
    for sku in skus:
        label, prefix = SKUS[sku]
        flags = " ".join(
            f"-p alp_{prefix}{cpu}/ae822fa0e5597ls0/{soc}" for cpu, soc in CPU.items()
        )
        # Two twister --subset shards per SKU: halves the leg wall time, and
        # spreads the slow HE builds (skewed heavy) across both legs.
        for subset in (1, 2):
            legs.append(
                {"sku": sku, "subset": subset, "label": label, "platform_flags": flags}
            )
    return legs


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", help="base revision; omit for the full matrix")
    ap.add_argument("--head", default="HEAD")
    ap.add_argument("--github-output", action="store_true",
                    help="append matrix=<json> to $GITHUB_OUTPUT")
    args = ap.parse_args(argv)

    paths: list[str] = []
    if args.base:
        try:
            diff = subprocess.run(
                ["git", "diff", "--name-only", "--no-renames", args.base, args.head],
                check=True, capture_output=True, text=True, encoding="utf-8",
            ).stdout
            paths = [p for p in diff.splitlines() if p]
        except (OSError, subprocess.CalledProcessError):
            paths = []  # fail safe: full matrix
    legs = matrix(affected_skus(paths))
    text = json.dumps(legs, separators=(",", ":"))
    print(f"select_aen_twister: {len(legs)} leg(s)", file=sys.stderr)
    if args.github_output:
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as fh:
            fh.write(f"matrix={text}\n")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
