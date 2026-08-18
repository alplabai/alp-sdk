#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
`_CHIP_SUBSYSTEMS` <-> Kconfig `depends on` parity gate -- fails (exit 1)
when `_CHIP_SUBSYSTEMS` in `scripts/alp_project_emit/__init__.py` drifts
from the `depends on` line of the matching `config ALP_SDK_CHIP_<NAME>` /
`config ALP_SDK_BLOCK_<NAME>` block in `zephyr/kconfigs/chips.kconfig`.

Root cause (issue #1487): `_CHIP_SUBSYSTEMS` is a hand-typed copy of each
config block's `depends on` line, consumed by `_emit_chips` /
`_emit_subsystems` (`scripts/alp_orchestrate/kconfig.py`) to turn on the
matching `CONFIG_<SUBSYS>=y` alongside every `CONFIG_ALP_SDK_CHIP_<NAME>=y`
it prints.  A miss is swallowed silently -- `chip_subsystems_table.get(chip,
())` just contributes nothing -- so Kconfig received the chip's `=y`
assignment with its dependency never turned on, and dropped it as an unmet
dependency (a warning, not a build error).  Ten of the eighty entries had no
key at all with nothing gating the drift; this script is that gate.

Correspondence rule:
    config ALP_SDK_CHIP_<NAME>  <-->  _CHIP_SUBSYSTEMS["<name>"]
    config ALP_SDK_BLOCK_<NAME> <-->  _CHIP_SUBSYSTEMS["<name>"]
    (`<name>` is `<NAME>` lower-cased; the `CHIP_`/`BLOCK_` prefix carries
    no information the table needs -- both map to the same bare slug, e.g.
    `ALP_SDK_BLOCK_BUTTON_LED` <-> `"button_led"`.)

A config block's `depends on <expr>` is reduced to the *set* of subsystem
tokens it names, regardless of `&&` / `||`: for an AND expression every
token must be on for the driver to build, and for the one `||` case in the
tree today (`ALP_SDK_CHIP_GD32G553`, `depends on (SPI || I2C)`) the table
deliberately turns both on rather than picking a side of the OR (see the
table's own comment) -- so in both cases the *expected* `_CHIP_SUBSYSTEMS`
value is exactly the token set the `depends on` line names.  A config with
no `depends on` line at all (e.g. `pdm_mic`, `rtl8211fdi`) expects an empty
set, which a present-but-empty tuple (`()`) or an absent key both satisfy --
`table.get(chip, ())` treats them identically at the call site this table
feeds.

This does NOT derive the table (the issue's stronger alternative fix); it
pins byte-for-byte parity against the Kconfig source of truth the table
already documents itself as copying, the same shape
`check_cmake_chip_list_parity.py` and `check_chip_manifest_parity.py` use
for their own driver<->registration parity gates.
"""
from __future__ import annotations

import argparse
import ast
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

_CONFIG_RE = re.compile(r"^config ALP_SDK_(?:CHIP|BLOCK)_([A-Z0-9_]+)$", re.MULTILINE)
_DEPENDS_RE = re.compile(r"^\s*depends on (.+)$", re.MULTILINE)
_TOKEN_RE = re.compile(r"[A-Z][A-Z0-9_]*")
# Any of these line-starts closes the current chip's block: another config
# entry (whether or not it's an ALP_SDK_CHIP/BLOCK_ one -- an intervening
# sub-option like `config ALP_SDK_CC3501E_REQUEST_LOCK_TIMEOUT_MS` must NOT
# donate its (absent, here) `depends on` line to the preceding chip), or an
# `if <chip>` / `endif` guard wrapping such a sub-option (chips.kconfig:128-153
# has exactly this shape around ALP_SDK_CHIP_CC3501E).
_BLOCK_END_RE = re.compile(r"^(?:config |if |endif\b)", re.MULTILINE)


def _kconfig_subsystems(root: Path) -> dict[str, frozenset[str]]:
    """Parse `zephyr/kconfigs/chips.kconfig`: slug -> expected subsystem set."""
    path = root / "zephyr" / "kconfigs" / "chips.kconfig"
    text = path.read_text(encoding="utf-8")
    matches = list(_CONFIG_RE.finditer(text))
    result: dict[str, frozenset[str]] = {}
    for m in matches:
        slug = m.group(1).lower()
        end_m = _BLOCK_END_RE.search(text, m.end())
        end = end_m.start() if end_m else len(text)
        block = text[m.end():end]
        # Union every `depends on` line in the block, not just the first --
        # Kconfig permits multiple ANDed `depends on` lines in one block.
        tokens: set[str] = set()
        for dep in _DEPENDS_RE.findall(block):
            tokens.update(_TOKEN_RE.findall(dep))
        result[slug] = frozenset(tokens)
    return result


def _chip_subsystems_table(root: Path) -> dict[str, frozenset[str]]:
    """Parse the `_CHIP_SUBSYSTEMS` dict literal out of
    `scripts/alp_project_emit/__init__.py` via `ast` -- NOT import, so this
    gate needs no importable package (the module does relative imports of
    its `.dts` / `.native_sim` / ... siblings, which a synthetic test tree
    need not provide)."""
    path = root / "scripts" / "alp_project_emit" / "__init__.py"
    text = path.read_text(encoding="utf-8")
    tree = ast.parse(text, filename=str(path))
    for node in ast.walk(tree):
        if (isinstance(node, ast.AnnAssign)
                and isinstance(node.target, ast.Name)
                and node.target.id == "_CHIP_SUBSYSTEMS"):
            raw: dict[str, tuple[str, ...]] = ast.literal_eval(node.value)
            return {k: frozenset(v) for k, v in raw.items()}
    raise SystemExit(f"error: no `_CHIP_SUBSYSTEMS: dict[...] = {{...}}` "
                      f"assignment found in {path}")


def find_problems(root: Path) -> list[str]:
    kconfig = _kconfig_subsystems(root)
    table = _chip_subsystems_table(root)
    errors: list[str] = []

    for slug, expected in sorted(kconfig.items()):
        actual = table.get(slug)
        if actual is None:
            if expected:
                errors.append(
                    f"'{slug}' depends on {sorted(expected)} in "
                    f"zephyr/kconfigs/chips.kconfig but has no "
                    f"_CHIP_SUBSYSTEMS key -- add "
                    f"\"{slug}\": {tuple(sorted(expected))!r} to "
                    f"scripts/alp_project_emit/__init__.py")
            continue
        if actual != expected:
            errors.append(
                f"'{slug}' depends on {sorted(expected)} in "
                f"zephyr/kconfigs/chips.kconfig but _CHIP_SUBSYSTEMS has "
                f"{sorted(actual)} -- update the entry in "
                f"scripts/alp_project_emit/__init__.py to match")

    stale = set(table) - set(kconfig)
    for slug in sorted(stale):
        errors.append(
            f"'{slug}' has a _CHIP_SUBSYSTEMS entry but no matching "
            f"config ALP_SDK_CHIP_{slug.upper()} / "
            f"ALP_SDK_BLOCK_{slug.upper()} in "
            f"zephyr/kconfigs/chips.kconfig -- remove the stale entry from "
            f"scripts/alp_project_emit/__init__.py")

    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO, help="repo root (for tests)")
    args = ap.parse_args()

    errors = find_problems(args.root)
    if errors:
        print("chip-subsystems parity check FAILED:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    print("chip-subsystems parity OK: _CHIP_SUBSYSTEMS matches every "
          "config ALP_SDK_CHIP_*/ALP_SDK_BLOCK_* `depends on` line.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
