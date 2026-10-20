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

When every non-neutral changed path sits inside an example that twister
builds here (the nearest directory holding a testcase.yaml lies under one of
ROOTS), the PR is narrowed: each SKU runs ONE leg over just those example
directories instead of SUBSETS legs over every root (#2885).  Anything else --
shared code, a file outside every example, more than NARROW_MAX examples --
runs the full split.

Prints the matrix as a JSON list for `strategy.matrix.include`: one entry per
SKU x twister `--subset i/SUBSETS` shard, each carrying the `--testsuite-root`
arguments it builds.  An empty list means "run nothing".
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

SKUS = {
    "aen801": ("AEN801 (M55-HE + M55-HP)", "e1m_aen801_m55_"),
    "aen803": ("AEN803 (M55-HE + M55-HP)", "e1m_aen803_m55_"),
}
CPU = {"he": "rtss_he", "hp": "rtss_hp"}
NEUTRAL_PREFIXES = ("changelog.d/", "docs/", "tests/scripts/")

# Every --testsuite-root this job builds (the workflow reads them from the
# matrix; this is the only list).  Each needs a matching `<root>/**` entry in
# both of the workflow's paths: filters -- test_select_aen_twister pins that.
# Why each is here: see the comment block above the twister step.
ROOTS = (
    "examples/aen",
    "examples/connectivity/firmware-update-log",
    "examples/connectivity/iot-dashboard",
    "examples/peripheral-io/drone-autopilot",
    "examples/connectivity/mqtt-telemetry",
    "examples/connectivity/iot-fleet-ota",
    "examples/connectivity/camera-mjpeg-stream",
    "examples/peripheral-io/alp-console",
    "examples/peripheral-io/blink",
    "examples/peripheral-io/pwm-led-fade",
    "examples/peripheral-io/qenc-readout",
    "examples/ai/ai-anomaly-detection-vibration",
    "examples/ai/motor-current-signature",
    "examples/ai/multimodal-fusion-pdm",
    "examples/ai/rail-predictive-maintenance",
    "examples/ai/visual-defect-detection",
    "examples/ai/wearable-activity-fall",
    "examples/audio/acoustic-anomaly-wind-turbine",
    "examples/audio/acoustic-safety-events",
    "examples/audio/audio-wake-word",
    "examples/audio/butterworth-lowpass",
    "examples/camera-vision/ai-camera-viewer",
    "examples/connectivity/production-deployment",
    "examples/display/drone-hud",
    "examples/display/lvgl-benchmark",
    "examples/display/lvgl-music-player",
    "examples/display/lvgl-widgets-demo",
    "examples/multicore/mproc-mailbox",
)

# A PR touching more examples than this runs the full split instead: past a
# handful, one unsplit leg would be slower than the shards.
NARROW_MAX = 10


def _exclusive_to(path: str) -> str | None:
    for sku, (_, prefix) in SKUS.items():
        if path.startswith(f"zephyr/boards/alp/{prefix}"):
            return sku
    return None


def _neutral(path: str) -> bool:
    return path.endswith(".md") or path.startswith(NEUTRAL_PREFIXES)


def _example_dir(path: str, repo: Path) -> str | None:
    """The nearest directory above `path` holding a testcase.yaml, or None
    when no directory on the way up has one (a shared file: full run)."""
    parts = path.split("/")
    for end in range(len(parts) - 1, 1, -1):
        d = "/".join(parts[:end])
        if (repo / d / "testcase.yaml").is_file():
            return d
    return None


def _in_roots(d: str) -> bool:
    return any(d == r or d.startswith(r + "/") for r in ROOTS)


def narrowed_roots(paths: list[str], repo: Path) -> list[str] | None:
    """The example directories to build when the change is examples-only,
    else None (full run).  An example outside ROOTS is not built by this job
    at all, so it narrows to nothing.  Empty list: nothing here to build."""
    dirs: set[str] = set()
    for p in paths:
        if _neutral(p):
            continue
        d = _example_dir(p, repo) if p.startswith("examples/") else None
        if d is None:
            return None
        if _in_roots(d):
            dirs.add(d)
    if len(dirs) > NARROW_MAX:
        return None
    return sorted(dirs)


def root_args(roots) -> str:
    return " ".join(f"--testsuite-root alp-sdk/{r}" for r in roots)


def affected_skus(paths: list[str]) -> list[str]:
    if not paths:
        return list(SKUS)
    out = []
    for sku in SKUS:
        for p in paths:
            if _neutral(p):
                continue
            if _exclusive_to(p) not in (None, sku):
                continue
            out.append(sku)
            break
    return out


# Twister --subset shards per SKU.  A leg is ~100 build-only configurations;
# a cold-ccache run (a PR touching a shared backend rebuilds everything) was
# measured at 26-55 of 62 configurations in the 34 min step cap with two
# shards (#2848).  Four shards still left AEN803 4/4 at 23 of 31 when it timed
# out (one build alone took 13 min, and the sorted split clusters the heavy
# HE builds), so six: ~17 configurations a leg.  The workflow also passes
# --shuffle-tests with a fixed seed so heavy builds do not cluster in one
# shard (#2885); with that, more shards do not lower the slowest one.  The workflow
# reads the count from the matrix (`matrix.subsets`), so this is the only
# place to change it.
SUBSETS = 6


def matrix(skus: list[str], roots: list[str] | None = None) -> list[dict]:
    """One leg per SKU x shard.  `roots` narrows to those example directories
    in one unsplit leg per SKU; None builds every root in SUBSETS shards."""
    subsets = 1 if roots else SUBSETS
    args = root_args(roots or ROOTS)
    legs = []
    for sku in skus:
        label, prefix = SKUS[sku]
        flags = " ".join(
            f"-p alp_{prefix}{cpu}/ae822fa0e5597ls0/{soc}" for cpu, soc in CPU.items()
        )
        # SUBSETS twister --subset shards per SKU: divides the leg wall time and
        # spreads the slow HE builds (skewed heavy) across the legs.
        for subset in range(1, subsets + 1):
            legs.append(
                {
                    "sku": sku,
                    "subset": subset,
                    "subsets": subsets,
                    "label": label,
                    "platform_flags": flags,
                    "roots": args,
                }
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
    roots = narrowed_roots(paths, REPO) if paths else None
    if roots == []:
        legs = []
    else:
        legs = matrix(affected_skus(paths), roots)
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
