#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pure reader for metadata/quality-tasks-v1.json (epic #610 §5).

The registry is the single source of truth for the SDK's quality gates;
scripts/test-all.sh fills its REQUIRED_GATE_SCRIPTS from `--gate-scripts` here,
so the wrapper and the registry (drift-gated in CI) can't diverge. stdlib only.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

_HERE = Path(__file__).resolve()
REGISTRY = _HERE.parent.parent / "metadata" / "quality-tasks-v1.json"


def load() -> dict:
    return json.loads(REGISTRY.read_text(encoding="utf-8"))


def _check_tasks(reg: dict) -> list[dict]:
    return [t for t in reg["tasks"] if t.get("runner") == "check-script"]


def check_scripts() -> list[str]:
    return sorted(t["script"] for t in _check_tasks(load()))


def gate_scripts() -> list[str]:
    return sorted(t["script"] for t in _check_tasks(load()) if t.get("gate"))


def scripts_for_profile(profile: str) -> list[str]:
    return sorted(t["script"] for t in _check_tasks(load())
                  if profile in t.get("profiles", []))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="read the quality-task registry")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--gate-scripts", action="store_true",
                   help="print each hard-gate check-script path, one per line")
    g.add_argument("--profile", help="print each check-script in the profile")
    args = ap.parse_args(argv)
    paths = gate_scripts() if args.gate_scripts else scripts_for_profile(args.profile)
    for p in paths:
        print(p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
