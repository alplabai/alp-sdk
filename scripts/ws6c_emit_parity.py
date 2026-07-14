#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Emit-parity harness for the WS6-c library-resolution rewrite (#610 §6).

Captures the `--emit zephyr-conf` output for every library-using example x core
into a golden baseline, then (post-rewrite) diffs the live emit against it. The
resolver rewrite MUST preserve every emitted CONFIG_ line -- a silent drop
breaks real firmware. This harness is the gate.

    python3 scripts/ws6c_emit_parity.py --capture   # write the golden baseline
    python3 scripts/ws6c_emit_parity.py --check      # diff live emit vs golden

Golden lives at tests/fixtures/ws6c-emit-parity/<example>__<core>.conf (only the
library-relevant CONFIG lines, sorted, so unrelated ordering churn is ignored).
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / "tests/fixtures/ws6c-emit-parity"


def _targets() -> list[tuple[Path, str]]:
    """(board.yaml, core) for every example core that declares libraries."""
    out: list[tuple[Path, str]] = []
    for by in sorted(ROOT.glob("examples/*/*/board.yaml")):
        doc = yaml.safe_load(by.read_text(encoding="utf-8")) or {}
        cores = doc.get("cores") or {}
        for cid, centry in cores.items():
            if isinstance(centry, dict) and centry.get("libraries"):
                out.append((by, cid))
        # project-wide libraries: emit against every core
        if doc.get("libraries"):
            for cid in cores:
                if (by, cid) not in out:
                    out.append((by, cid))
    return out


def _emit(board: Path, core: str) -> str | None:
    """Live `--emit zephyr-conf` for one example core, reduced to the library-
    relevant CONFIG lines (sorted). None if the emit itself fails."""
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts/alp_project.py"),
         "--input", str(board), "--emit", "zephyr-conf", "--core", core],
        capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        return None
    # Keep every CONFIG line (library CONFIG_ can't be reliably prefix-filtered
    # -- e.g. CONFIG_LVGL, CONFIG_MBEDTLS -- so compare the full sorted config
    # surface; unrelated churn would be a real diff worth seeing anyway).
    lines = sorted(l.rstrip() for l in r.stdout.splitlines()
                   if l.strip() and not l.lstrip().startswith("#"))
    return "\n".join(lines) + "\n"


def _key(board: Path, core: str) -> str:
    return f"{board.parent.relative_to(ROOT / 'examples').as_posix().replace('/', '__')}__{core}"


def capture() -> int:
    GOLDEN.mkdir(parents=True, exist_ok=True)
    n = skipped = 0
    for board, core in _targets():
        emit = _emit(board, core)
        if emit is None:
            print(f"skip (emit failed): {board.parent.name}/{core}", file=sys.stderr)
            skipped += 1
            continue
        (GOLDEN / f"{_key(board, core)}.conf").write_text(emit, encoding="utf-8")
        n += 1
    print(f"captured {n} baseline(s), {skipped} skipped -> {GOLDEN.relative_to(ROOT)}")
    return 0


def check() -> int:
    drift = 0
    for board, core in _targets():
        golden = GOLDEN / f"{_key(board, core)}.conf"
        if not golden.is_file():
            continue  # not baselined (e.g. added post-capture)
        live = _emit(board, core)
        if live is None:
            print(f"PARITY FAIL: {board.parent.name}/{core}: emit now errors", file=sys.stderr)
            drift += 1
            continue
        if live != golden.read_text(encoding="utf-8"):
            print(f"PARITY FAIL: {board.parent.name}/{core}: emit changed vs golden", file=sys.stderr)
            drift += 1
    if drift:
        print(f"{drift} example core(s) drifted from the pre-rewrite emit", file=sys.stderr)
        return 1
    print("OK: live emit matches the golden baseline for every example core.")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="WS6-c emit-parity harness")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--capture", action="store_true")
    g.add_argument("--check", action="store_true")
    args = ap.parse_args(argv)
    return capture() if args.capture else check()


if __name__ == "__main__":
    sys.exit(main())
