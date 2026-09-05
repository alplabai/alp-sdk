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

The A32 (Cortex-A32) Linux case (#1981)
---------------------------------------
The measured A32 chain -- Secure Enclave -> TF-A BL32 (SP_MIN, AArch32,
XIP at 0x80002000) -> Linux as BL33 (xipImage at 0x80020000), with the
carrier .dtb and the cramfs rootfs also resident in MRAM -- boots to a
shell on E1M-AEN803 and needs `cpu_id: "A32_0"` on its `mramAddress`
entries. That chain runs from 0x80002000 up to the APP Package, i.e. it
covers BOTH M55 slot0 windows in full. That is the intended layout, not
a #1069-style collision: an A32 Linux system uses essentially the whole
App MRAM, and the M55 entries in such an ATOC are `loadAddress` ITCM
stubs (0x50000000 / 0x58000000), never MRAM residents.

So the two layouts are MUTUALLY EXCLUSIVE, and that is what this module
enforces: if any `mramAddress` entry declares an A32 cpu_id, no
`mramAddress` entry may declare an M55 cpu_id, and vice versa.
`loadAddress` (ITCM) entries stay exempt exactly as above.

The A32 window's top is deliberately NOT a policy constant the way the
M55 windows are. The M55 windows are fixed generator policy (they
mirror the module YAML), but the A32 region ends at the APP Package
start, which MOVES with the package size -- SETOOLS reports the actual
value per build in `build/app-package-map.txt` (0x8057a8f0 for the
22288-byte package measured on 2026-09-05). Hardcoding that address
here would assert a fixed boundary that isn't one, so this guard checks
only the two bounds it can state as facts: the A32 base 0x80002000 and
the hard MRAM ceiling MRAM_END. An entry between the real package start
and MRAM_END is therefore NOT caught here -- SETOOLS' own package map
is the check for that.

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

# (base, size_bytes) per M55 cpu_id -- see module docstring; mirrors
# metadata/e1m_modules/E1M-AEN801.yaml `memory_map:` he_slot0 / hp_slot0.
# M55 ONLY, deliberately: scripts/west_commands/runners/alif_flash.py
# scans this dict to map a build's reset vector back to its core, and an
# A32 region (which spans both M55 windows) would swallow every vector.
SLOT0_WINDOWS = {
    'M55_HE': (0x80010000, 2688 * 1024),
    'M55_HP': (0x802b0000, 2688 * 1024),
}

# System MRAM end (bench-confirmed; matches alif_flash.py's prior
# _SLOT0_REGION_END and the top of metadata's `storage` region).
MRAM_END = 0x80580000

# cpu_id values the Secure Enclave boots on the Cortex-A32 (#1981).
A32_CPU_IDS = frozenset({'A32_0'})

# Where the A32 chain starts: TF-A BL32 (SP_MIN) XIPs from here, the SE
# boot table's BOOTLOAD entry points at it. Unlike SLOT0_WINDOWS there
# is no matching *top* constant -- the A32 region ends at the APP
# Package start, which moves with the package size (see module
# docstring); MRAM_END is the only honest ceiling this module has.
A32_BASE = 0x80002000


class AtocValidationError(ValueError):
    """An assembled ATOC config violates the #1069 window/overlap guard."""


def validate_atoc_entries(entries: "dict[str, Any]") -> None:
    """Validate the `mramAddress` entries in an assembled ATOC config.

    *entries* is the config dict as written for `app-gen-toc` (entry
    name -> {cpu_id, mramAddress|loadAddress, flags, ...}); the
    `DEVICE` entry (no mramAddress) is ignored automatically, as is any
    `loadAddress` (ITCM) entry. Each `mramAddress` entry must sit in its
    cpu_id's region (per-core M55 slot0 window, or the A32 region), no
    two may share an address, and A32 and M55 `mramAddress` entries are
    mutually exclusive (#1981 -- see module docstring). Raises
    AtocValidationError on the first violation found.
    """
    mram_entries: "list[tuple[str, int, str]]" = []  # (name, base, cpu_id)
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
        if cpu_id in A32_CPU_IDS:
            # Base bound only -- the top is the APP Package start, which
            # is per-build, not policy (see module docstring). The
            # MRAM_END ceiling was already enforced above.
            if base < A32_BASE:
                raise AtocValidationError(
                    f"ATOC entry '{name}' mramAddress 0x{base:x} falls below "
                    f'the {cpu_id} MRAM region (starts at 0x{A32_BASE:x}, the '
                    f'TF-A BL32 XIP base; ceiling is 0x{MRAM_END:x})')
        else:
            window = SLOT0_WINDOWS.get(cpu_id)
            if window is None:
                raise AtocValidationError(
                    f"ATOC entry '{name}' has unknown cpu_id {cpu_id!r} for a "
                    f'slot0-XIP window check -- expected one of '
                    f'{sorted(SLOT0_WINDOWS) + sorted(A32_CPU_IDS)}')
            win_base, win_size = window
            win_top = win_base + win_size
            if not (win_base <= base < win_top):
                raise AtocValidationError(
                    f"ATOC entry '{name}' mramAddress 0x{base:x} falls outside "
                    f'the {cpu_id} slot0 window (0x{win_base:x}..0x{win_top:x})')

        mram_entries.append((name, base, cpu_id))

    # A32-vs-M55 exclusivity (#1981): the A32 chain spans both M55 slot0
    # windows by design, so an ATOC that stages MRAM-resident images for
    # both is a layout the SE cannot boot, not a tighter #1069 overlap.
    a32_names = [n for n, _base, cid in mram_entries if cid in A32_CPU_IDS]
    m55_names = [n for n, _base, cid in mram_entries if cid not in A32_CPU_IDS]
    if a32_names and m55_names:
        raise AtocValidationError(
            f"ATOC stages both an A32 mramAddress entry ('{a32_names[0]}') "
            f"and an M55 slot0 entry ('{m55_names[0]}') -- the A32 Linux "
            'chain covers both M55 slot0 windows in full, so the two are '
            'mutually exclusive (#1981); M55 images in an A32 ATOC must be '
            'loadAddress (ITCM) entries')

    seen: "dict[int, str]" = {}
    for name, base, _cpu_id in mram_entries:
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
