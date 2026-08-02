#!/usr/bin/env python3
# Copyright (c) 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Shared AEN801 ATOC-assembly guard (#1069).

Every AEN801 flash path assembles a signed ATOC config (a JSON object of
``{entry_name: {cpu_id, [mramAddress|loadAddress], flags, ...}}``) and
hands it to SETOOLS' ``app-gen-toc``. Several independent tools build
that config (``grep -rln app-gen-toc scripts/`` finds every call site);
before #1069 the ones that stage an ``mramAddress`` (slot0-XIP) entry --
``scripts/west_commands/runners/alif_flash.py`` (single-entry, ``west
flash``) and ``scripts/bench/aen/flash-run-dualcore.sh`` (two-entry, the
bench dual-core recipe) -- trusted a single shared ``mramAddress``
constant that was actually the same address for both M55 cores (the bug
this issue fixes). This module is the ONE place that validates an
assembled ATOC config before ``app-gen-toc`` runs, so those callers
can't drift back apart. ``scripts/bench/aen/flash-jlink-mramxip.sh``
(Flow D, HE-only slot0-XIP) also calls it as of the #1100 review fix.
The remaining call sites (``flash-jlink.sh``, ``flash-jlink-hp.sh``,
``flash-run.sh``, ``flash-update-log-dual.sh``,
``flash-update-log-firewall-probe.sh``) stage ``loadAddress`` (ITCM)
entries only -- this guard is a deliberate no-op for those (see
``validate_atoc_entries`` below), so they don't call it.

Per-core App-MRAM slot0 windows (disjoint since #1069) -- mirrors the
`memory_map:` block in metadata/e1m_modules/E1M-AEN801.yaml. These are
generator-policy constants, not re-derived from that YAML at flash time
(the same convention scripts/gen_zephyr_board.py's own AEN generator
constants already use), so this module stays stdlib-only and importable
from a bare west/SETOOLS environment with no PyYAML dependency:

    M55_HE  0x80010000 .. 0x802b0000  (2688 KiB, unchanged since before #1069)
    M55_HP  0x802b0000 .. 0x80550000  (2688 KiB, moved off the old shared
                                        0x80010000 window)

Only `mramAddress` (slot0-XIP) entries are window/overlap-checked here.
`loadAddress` (ITCM) entries -- the shape every current AEN dual-core
*example* uses -- are already disjoint by construction (0x50000000
M55-HP vs 0x58000000 M55-HE) and are left alone; a mixed config (one
ITCM entry + one slot0-XIP entry) is legitimate and not rejected.

Two known gaps, NOT closed by this guard (see #1069's PR body):
  - A sequential single-core `west flash` writes a whole fresh TOC each
    run, so the previous core's entry is invisible to this guard by the
    time the second `west flash` runs -- catching that needs a pre-burn
    TOC read-back over the SE-UART.
  - The J-Link customer path (loadbin straight to slot0) has no ATOC
    assembly step at all; for that path the disjoint DTS windows ARE
    the guard (the link address itself moves per core).
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

# (base, size_bytes) per cpu_id -- see module docstring; mirrors
# metadata/e1m_modules/E1M-AEN801.yaml `memory_map:` he_slot0 / hp_slot0.
SLOT0_WINDOWS = {
    'M55_HE': (0x80010000, 2688 * 1024),
    'M55_HP': (0x802b0000, 2688 * 1024),
}

# System MRAM end (bench-confirmed; matches alif_flash.py's prior
# _SLOT0_REGION_END and the top of metadata's `storage` region).
MRAM_END = 0x80580000


class AtocValidationError(ValueError):
    """An assembled ATOC config violates the #1069 window/overlap guard."""


def validate_atoc_entries(entries: "dict[str, Any]") -> None:
    """Validate the `mramAddress` entries in an assembled ATOC config.

    *entries* is the config dict as written for `app-gen-toc` (entry
    name -> {cpu_id, mramAddress|loadAddress, flags, ...}); the
    `DEVICE` entry (no mramAddress) is ignored automatically, as is any
    `loadAddress` (ITCM) entry. Raises AtocValidationError on the first
    violation found.
    """
    mram_entries: "list[tuple[str, int]]" = []  # (name, base)
    for name, entry in entries.items():
        if not isinstance(entry, dict) or 'mramAddress' not in entry:
            continue  # DEVICE entry, or a loadAddress (ITCM) entry
        addr = entry['mramAddress']
        try:
            base = int(addr, 16) if isinstance(addr, str) else int(addr)
        except (TypeError, ValueError):
            raise AtocValidationError(
                f"ATOC entry '{name}' has a non-numeric mramAddress {addr!r}")

        if base >= MRAM_END or base < 0x80000000:
            raise AtocValidationError(
                f"ATOC entry '{name}' mramAddress 0x{base:x} is outside "
                f'System MRAM (0x80000000..0x{MRAM_END:x})')

        cpu_id = entry.get('cpu_id')
        window = SLOT0_WINDOWS.get(cpu_id)
        if window is None:
            raise AtocValidationError(
                f"ATOC entry '{name}' has unknown cpu_id {cpu_id!r} for a "
                f'slot0-XIP window check -- expected one of '
                f'{sorted(SLOT0_WINDOWS)}')
        win_base, win_size = window
        win_top = win_base + win_size
        if not (win_base <= base < win_top):
            raise AtocValidationError(
                f"ATOC entry '{name}' mramAddress 0x{base:x} falls outside "
                f'the {cpu_id} slot0 window (0x{win_base:x}..0x{win_top:x})')

        mram_entries.append((name, base))

    seen: "dict[int, str]" = {}
    for name, base in mram_entries:
        prior = seen.get(base)
        if prior is not None:
            raise AtocValidationError(
                f"ATOC entries '{prior}' and '{name}' both stage to the "
                f'same mramAddress 0x{base:x} -- flashing both would '
                'silently overwrite one image (#1069)')
        seen[base] = name


def validate_atoc_config_file(path: "str | Path") -> None:
    """Load + validate a staged ATOC JSON config file."""
    data = json.loads(Path(path).read_text(encoding='utf-8'))
    validate_atoc_entries(data)


def main(argv: "list[str] | None" = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print('usage: aen_atoc.py <staged-atoc-config.json>', file=sys.stderr)
        return 2
    try:
        validate_atoc_config_file(argv[0])
    except (AtocValidationError, OSError, json.JSONDecodeError) as exc:
        print(f'aen_atoc: REJECTED: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
