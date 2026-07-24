#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
CI schema/smoke contract for `--emit kconfig` (#893).

`--emit kconfig` is the SDK's first workspace-dependent emit mode (it
needs a bootstrapped `ZEPHYR_BASE`, v4.4.0) -- deliberately OUTSIDE
`scripts/check_emit_snapshots.py` (the hermetic byte-golden gate every
other `--emit` surface is pinned against). Run this from a job that
already bootstrapped the Zephyr workspace (the `pr-twister` CI job); it
is a no-op you should not attempt without one.

Unlike a byte-golden, this gate is schema/smoke only: the exact symbol
set moves with the pinned Zephyr version, so it asserts SHAPE, never a
frozen dump --

  * valid JSON, `schemaVersion == 1`, `symbols` non-empty;
  * every symbol carries `name` + `type`, `type` in the allowed set;
  * a few known-must-exist symbols are present (core Zephyr subsystems
    every image can enable, regardless of board);
  * the envelope's + every symbol's KEY SET conforms to the canonical
    cross-repo contract fixture `tests/fixtures/kconfig-contract/
    emit-kconfig.golden.json` -- tan-cli's `parse_kconfig` and
    alp-sdk-vscode's `kconfigSymbolsFromEnvelope` both test against the
    same file, so a key rename here needs a `schemaVersion` bump +
    coordinated updates there (key names/shape only -- never values or
    symbol counts, which legitimately vary with the pinned Zephyr version).

Usage (from a job with ZEPHYR_BASE + west + the Zephyr Python deps on
PATH):

    python3 scripts/check_emit_kconfig_contract.py
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

_GOLDEN = REPO / "tests" / "fixtures" / "kconfig-contract" / "emit-kconfig.golden.json"

_ALLOWED_TYPES = {"bool", "tristate", "int", "hex", "string"}

# (board.yaml, core).  One real AEN core is enough to prove the contract
# end-to-end (Approach A's stub-app load is board-target-agnostic) without
# paying for a per-SKU toolchain matrix in CI.
_CASES = [
    ("examples/aen/aen-eeprom-manifest/board.yaml", "m55_he"),
]

# Symbols the resolved Kconfig tree always carries regardless of board
# (core Zephyr subsystems every image can enable) -- their absence means
# the Approach-A load silently resolved the wrong tree, not that this
# particular board happens to lack them.
_KNOWN_SYMBOLS = {"LOG", "SERIAL", "MAIN_STACK_SIZE"}


def _golden_key_sets() -> tuple[set[str], set[str]]:
    """(envelope key set, symbol key set) from the canonical cross-repo
    contract fixture (tests/fixtures/kconfig-contract/README.md) -- tan-cli's
    `parse_kconfig` and alp-sdk-vscode's `kconfigSymbolsFromEnvelope` both
    test their own parsers against the same file."""
    golden = json.loads(_GOLDEN.read_text(encoding="utf-8"))
    return set(golden.keys()), set(golden["symbols"][0].keys())


def _check_key_set_conformance(envelope: dict, label: str) -> list[str]:
    """The real emit's KEY SET (never values/counts, which legitimately vary
    with the pinned Zephyr version) must conform to the golden fixture's
    shape -- a silent field rename here would break tan-cli and
    alp-sdk-vscode's parsers without either repo's own hand-written literal
    ever catching it (the whole point of #893's shared fixture)."""
    problems: list[str] = []
    golden_envelope_keys, golden_symbol_keys = _golden_key_sets()

    envelope_keys = set(envelope.keys())
    if envelope_keys != golden_envelope_keys:
        problems.append(
            f"{label}: envelope key set drifted from "
            f"{_GOLDEN.relative_to(REPO)} "
            f"(extra={sorted(envelope_keys - golden_envelope_keys)}, "
            f"missing={sorted(golden_envelope_keys - envelope_keys)}); "
            f"bump schemaVersion + update tan-cli/alp-sdk-vscode if this "
            f"is intentional")

    sym_key_sets = [set(sym.keys()) for sym in (envelope.get("symbols") or [])]
    union_keys = set().union(*sym_key_sets) if sym_key_sets else set()
    common_keys = set.intersection(*sym_key_sets) if sym_key_sets else set()
    extra = union_keys - golden_symbol_keys
    missing = golden_symbol_keys - common_keys
    if extra or missing:
        problems.append(
            f"{label}: symbol key set drifted from "
            f"{_GOLDEN.relative_to(REPO)} "
            f"(extra={sorted(extra)}, missing={sorted(missing)}); "
            f"bump schemaVersion + update tan-cli/alp-sdk-vscode if this "
            f"is intentional")

    return problems


def _run_case(board_yaml: str, core: str) -> list[str]:
    env = {**os.environ}
    scripts_dir = str(REPO / "scripts")
    env["PYTHONPATH"] = (
        scripts_dir + os.pathsep + env["PYTHONPATH"]
        if env.get("PYTHONPATH") else scripts_dir
    )
    cmd = [sys.executable, "-m", "alp_orchestrate",
           "--input", str(REPO / board_yaml),
           "--emit", "kconfig", "--core", core]
    proc = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, env=env)
    label = f"{board_yaml}::{core}"

    if proc.returncode != 0:
        return [f"{label}: --emit kconfig exited {proc.returncode}: "
                f"{proc.stderr.strip()}"]

    try:
        envelope = json.loads(proc.stdout)
    except json.JSONDecodeError as e:
        return [f"{label}: stdout is not valid JSON ({e})"]

    problems: list[str] = []
    if envelope.get("schemaVersion") != 1:
        problems.append(f"{label}: schemaVersion != 1 "
                         f"({envelope.get('schemaVersion')!r})")
    if envelope.get("core") != core:
        problems.append(f"{label}: core != {core!r} "
                         f"({envelope.get('core')!r})")

    symbols = envelope.get("symbols")
    if not symbols:
        problems.append(f"{label}: symbols is empty")
        return problems

    names: set[str] = set()
    for sym in symbols:
        name = sym.get("name")
        sym_type = sym.get("type")
        if not name:
            problems.append(f"{label}: a symbol entry has no name: {sym!r}")
            continue
        names.add(name)
        if not sym_type:
            problems.append(f"{label}: symbol {name} has no type")
        elif sym_type not in _ALLOWED_TYPES:
            problems.append(
                f"{label}: symbol {name} has type {sym_type!r}, not one "
                f"of {sorted(_ALLOWED_TYPES)}")

    missing_known = _KNOWN_SYMBOLS - names
    if missing_known:
        problems.append(
            f"{label}: expected known symbol(s) missing: "
            f"{sorted(missing_known)}")

    # #893 cross-repo contract: the real emit's key shape must conform to
    # the golden fixture tan-cli + alp-sdk-vscode also test against
    # (tests/fixtures/kconfig-contract/README.md). Key names/shape only --
    # never values or symbol counts, which legitimately vary with the
    # pinned Zephyr version.
    problems += _check_key_set_conformance(envelope, label)

    return problems


def main() -> int:
    argparse.ArgumentParser(description=__doc__).parse_args()

    # Not a failure: this contract needs the real Zephyr workspace this
    # script's own docstring says it does. `python scripts/alp_quality.py
    # --profile pr` (and any other local-first sweep) must stay green
    # without one -- the real gate runs in pr-twister.yml, which sets
    # ZEPHYR_BASE.  Mirrors tests/scripts/test_emit_kconfig_workspace.py's
    # skipif.
    if not os.environ.get("ZEPHYR_BASE"):
        print("skipped: --emit kconfig contract needs ZEPHYR_BASE")
        return 0

    all_problems: list[str] = []
    for board_yaml, core in _CASES:
        label = f"{board_yaml}::{core}"
        problems = _run_case(board_yaml, core)
        all_problems += problems
        if problems:
            print(f"FAIL {label}")
            for p in problems:
                print(f"  · {p}")
        else:
            print(f"OK   {label}")

    if all_problems:
        return 1
    print(f"check_emit_kconfig_contract: {len(_CASES)} case(s) OK.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
