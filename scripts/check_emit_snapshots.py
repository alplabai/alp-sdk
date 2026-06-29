#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Byte-for-byte `--emit` snapshot regression gate.

The planner of record (`scripts/alp_orchestrate.py`) is being modularized
into a layered package (issue #285).  A refactor must change *shape*, never
*behaviour*: the `--emit` surfaces the IDE/CLI consume (ADR 0014) have to
stay byte-identical.  This gate pins them.

For each (board.yaml, emit-mode) case it runs the emitter and compares the
output, byte-for-byte, to a committed golden under
`tests/fixtures/emit-snapshots/`.  Machine-specific absolute paths (the SDK
checkout root) are normalised to `<SDK_ROOT>` so the goldens are portable.

This is the explicit behaviour-pin the #285 extractions land against: an
extraction that moves code but keeps `--emit` identical is green here; one
that alters a byte fails immediately.

Usage:

    python3 scripts/check_emit_snapshots.py            # verify against goldens
    python3 scripts/check_emit_snapshots.py --update     # (re)write the goldens
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SNAP_DIR = REPO / "tests" / "fixtures" / "emit-snapshots"
ORCH = REPO / "scripts" / "alp_orchestrate.py"
PROJ = REPO / "scripts" / "alp_project.py"

# (snapshot id, tool, board.yaml, emit mode).  Deterministic, write-free
# emits across the three vendor families -- the contract surfaces a refactor
# must preserve.  build-plan carries the SDK-root abs path (normalised below).
CASES = [
    ("rpmsg-aen.system-manifest",   ORCH, "examples/multicore/rpmsg-aen/board.yaml",            "system-manifest"),
    ("rpmsg-aen.build-plan",        ORCH, "examples/multicore/rpmsg-aen/board.yaml",            "build-plan"),
    ("rpmsg-v2n.system-manifest",   ORCH, "examples/multicore/rpmsg-v2n/board.yaml",            "system-manifest"),
    ("rpmsg-v2n.build-plan",        ORCH, "examples/multicore/rpmsg-v2n/board.yaml",            "build-plan"),
    ("rpmsg-imx93.system-manifest", ORCH, "examples/multicore/rpmsg-imx93/board.yaml",          "system-manifest"),
    ("rpmsg-imx93.build-plan",      ORCH, "examples/multicore/rpmsg-imx93/board.yaml",          "build-plan"),
    ("hetero-offload.system-manifest", ORCH, "examples/multicore/heterogeneous-offload/board.yaml", "system-manifest"),
    ("hetero-offload.build-plan",      ORCH, "examples/multicore/heterogeneous-offload/board.yaml", "build-plan"),
]


def _emit(tool: Path, board: str, mode: str) -> str:
    """Run the emitter from the repo root; normalise the SDK-root path."""
    rv = subprocess.run(
        [sys.executable, str(tool), "--input", board, "--emit", mode],
        capture_output=True, text=True, cwd=REPO, check=False)
    if rv.returncode != 0:
        raise SystemExit(f"check_emit_snapshots: emit failed for {board} "
                         f"--emit {mode} (rc={rv.returncode}):\n{rv.stderr}")
    return rv.stdout.replace(str(REPO), "<SDK_ROOT>")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--update", action="store_true",
                    help="rewrite the golden snapshots instead of checking")
    args = ap.parse_args()
    SNAP_DIR.mkdir(parents=True, exist_ok=True)

    stale: list[str] = []
    for snap_id, tool, board, mode in CASES:
        got = _emit(tool, board, mode)
        golden = SNAP_DIR / f"{snap_id}.snap"
        if args.update:
            golden.write_text(got, encoding="utf-8")
            print(f"wrote {golden.relative_to(REPO)}")
            continue
        if not golden.is_file():
            stale.append(f"{snap_id}: no golden (run --update)")
            print(f"MISS {snap_id}")
        elif golden.read_text(encoding="utf-8") != got:
            stale.append(f"{snap_id}: emit differs from golden")
            print(f"DIFF {snap_id}")
        else:
            print(f"OK   {snap_id}")

    if args.update:
        print(f"\nupdated {len(CASES)} snapshot(s).")
        return 0
    if stale:
        print(f"\ncheck_emit_snapshots: {len(stale)} snapshot(s) changed -- a "
              f"refactor must keep --emit byte-identical.  If the change is "
              f"intentional, re-run with --update and review the diff.",
              file=sys.stderr)
        for s in stale:
            print(f"  · {s}", file=sys.stderr)
        return 1
    print(f"\ncheck_emit_snapshots: {len(CASES)} --emit surface(s) byte-identical.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
