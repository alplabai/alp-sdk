#!/usr/bin/env python3
# Copyright 2026 ALP Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Performance-baseline harness for the ALP SDK.

Reads `alp_bench`'s stdout (one line per microbench case, `<name> <iters>
iters <ns> ns/iter`), records the results into a per-target YAML file
under `tests/bench/baselines/<som>-<os>.yaml`, and -- when run with
`--diff` -- compares the current run against the on-disk baseline
and flags every case whose ns/iter is more than the configured
regression threshold worse.

v1.0 acceptance bar per VERSIONS.md: "Performance baselines per chip
in metadata."  This script is the harness; the captured baselines
live alongside the SDK so a CI gate can fail a PR that regresses any
case more than the threshold.

Usage
-----

    # Capture a fresh baseline on the current target.
    ./build/tests/bench/alp_bench | python3 tests/bench/baseline_runner.py \
        --record --som E1M-V2N101 --os yocto

    # Diff a current run against the recorded baseline.
    ./build/tests/bench/alp_bench | python3 tests/bench/baseline_runner.py \
        --diff --som E1M-V2N101 --os yocto

The threshold defaults to 10 %; bump via `--threshold-pct 20` for
noisier targets.
"""

from __future__ import annotations

import argparse
import datetime
import pathlib
import re
import sys
from typing import Optional

try:
    import yaml
except ImportError:  # pragma: no cover
    print("error: pyyaml not installed", file=sys.stderr)
    sys.exit(1)


HERE          = pathlib.Path(__file__).resolve().parent
ROOT          = HERE.parent.parent          # repo root (../.. from tests/bench/)
BASELINE_ROOT = HERE / "baselines"

# Output line from BENCH_RUN: <name> <iters> iters <ns> ns/iter
# (whitespace-tolerant).
_BENCH_LINE = re.compile(
    r"^\s*(\S+(?:\s+\S+)*?)\s+(\d+)\s+iters\s+(\d+)\s+ns/iter\s*$",
    re.MULTILINE,
)


def parse_bench_output(text: str) -> dict[str, dict[str, int]]:
    """Map name -> {iters, ns_per_iter} from alp_bench's stdout."""
    out: dict[str, dict[str, int]] = {}
    for m in _BENCH_LINE.finditer(text):
        name = m.group(1).strip()
        out[name] = {
            "iters":        int(m.group(2)),
            "ns_per_iter":  int(m.group(3)),
        }
    return out


def baseline_path(som: str, os_name: str) -> pathlib.Path:
    return BASELINE_ROOT / f"{som}-{os_name}.yaml"


def record(som: str, os_name: str,
           cases: dict[str, dict[str, int]],
           toolchain: Optional[str]) -> pathlib.Path:
    BASELINE_ROOT.mkdir(parents=True, exist_ok=True)
    path = baseline_path(som, os_name)
    doc = {
        "schema_version":  1,
        "som":             som,
        "os":              os_name,
        "toolchain":       toolchain or "unknown",
        "recorded_at":     datetime.date.today().isoformat(),
        "cases": [
            {
                "name":         name,
                "iters":        v["iters"],
                "ns_per_iter":  v["ns_per_iter"],
            }
            for name, v in sorted(cases.items())
        ],
    }
    path.write_text(yaml.safe_dump(doc, sort_keys=False), encoding="utf-8")
    return path


def load_baseline(som: str, os_name: str) -> Optional[dict[str, dict[str, int]]]:
    path = baseline_path(som, os_name)
    if not path.exists():
        return None
    with path.open(encoding="utf-8") as f:
        doc = yaml.safe_load(f)
    return {
        c["name"]: {"iters": c["iters"], "ns_per_iter": c["ns_per_iter"]}
        for c in (doc.get("cases") or [])
    }


def diff(current: dict[str, dict[str, int]],
         baseline: dict[str, dict[str, int]],
         threshold_pct: float) -> int:
    regressions: list[tuple[str, int, int, float]] = []
    improvements: list[tuple[str, int, int, float]] = []
    new_cases:    list[str] = []
    missing:      list[str] = []

    for name, cur in current.items():
        if name not in baseline:
            new_cases.append(name)
            continue
        b = baseline[name]["ns_per_iter"]
        c = cur["ns_per_iter"]
        if b == 0:
            continue
        delta_pct = 100.0 * (c - b) / b
        if delta_pct > threshold_pct:
            regressions.append((name, b, c, delta_pct))
        elif delta_pct < -threshold_pct:
            improvements.append((name, b, c, delta_pct))

    for name in baseline:
        if name not in current:
            missing.append(name)

    print()
    print(f"Threshold: ±{threshold_pct:.1f}%")
    print(f"Cases compared:   {len(current) - len(new_cases)}")
    print(f"Regressions:      {len(regressions)}")
    print(f"Improvements:     {len(improvements)}")
    print(f"New cases:        {len(new_cases)}")
    print(f"Missing in run:   {len(missing)}")

    for name, b, c, pct in sorted(regressions, key=lambda r: -r[3]):
        print(f"  REGRESS   {name:50s} {b:>6} ns -> {c:>6} ns  (+{pct:.1f}%)")
    for name in new_cases:
        print(f"  NEW       {name}  (no baseline -- record a refreshed baseline)")
    for name in missing:
        print(f"  MISSING   {name}  (baseline has this case; current run lacks it)")
    for name, b, c, pct in sorted(improvements, key=lambda r: r[3]):
        print(f"  better    {name:50s} {b:>6} ns -> {c:>6} ns  ({pct:+.1f}%)")

    return 1 if regressions else 0


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--record", action="store_true",
                      help="write the current run's parsed results to "
                           "the per-target baseline YAML")
    mode.add_argument("--diff", action="store_true",
                      help="compare the current run against the recorded "
                           "baseline; exit 1 on regression > threshold")
    parser.add_argument("--som", required=True,
                        help="SoM SKU label (e.g. E1M-V2N101)")
    parser.add_argument("--os", dest="os_name", required=True,
                        choices=["zephyr", "baremetal", "yocto"],
                        help="OS backend the bench was built against")
    parser.add_argument("--toolchain",
                        help="optional toolchain label baked into the baseline")
    parser.add_argument("--threshold-pct", type=float, default=10.0,
                        help="regression threshold in percent (diff mode; "
                             "default 10)")
    args = parser.parse_args(argv)

    text = sys.stdin.read()
    if not text.strip():
        print("error: no bench output on stdin", file=sys.stderr)
        return 1
    current = parse_bench_output(text)
    if not current:
        print("error: no recognisable bench lines in stdin "
              "(expected `<name> <N> iters <M> ns/iter`)",
              file=sys.stderr)
        return 1

    if args.record:
        path = record(args.som, args.os_name, current, args.toolchain)
        print(f"recorded {len(current)} case(s) to {path.relative_to(ROOT)}")
        return 0

    baseline = load_baseline(args.som, args.os_name)
    if baseline is None:
        print(f"error: no baseline at {baseline_path(args.som, args.os_name)}",
              file=sys.stderr)
        return 1

    return diff(current, baseline, args.threshold_pct)


if __name__ == "__main__":
    sys.exit(main())
