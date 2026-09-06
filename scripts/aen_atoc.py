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
    A32_0   0x80002000 .. 0x80578000  (5592 KiB, up to the base of the
                                        SE-owned `atoc` band -- #1981; an
                                        A32 Linux chain's mramAddress span
                                        runs from TF-A BL32 through the
                                        kernel and so owns essentially the
                                        whole App MRAM window below the
                                        boot table)

Only `mramAddress` (slot0-XIP) entries are window/overlap-checked here.
`loadAddress` (ITCM) entries -- the shape every current AEN dual-core
*example* uses -- are already disjoint by construction (0x50000000
M55-HP vs 0x58000000 M55-HE) and are left alone; a mixed config (one
ITCM entry + one slot0-XIP entry) is legitimate and not rejected.

#1981: an `A32_0` `mramAddress` entry and an `M55_HE`/`M55_HP`
`mramAddress` entry may never coexist in the same config -- the A32
window above entirely covers both M55 windows, so mixing them would
stage an M55 slot0 image into memory the A32 chain is executing from.
`loadAddress` (ITCM) M55 stub entries are exempt, same as above. That
mutual-exclusivity check only fires when an M55 `mramAddress` entry is
ALSO present -- the real, shipped A32 Linux boot config stages its M55
entries as `loadAddress` only (see `test_a32_linux_boot_config_passes`),
so the check never fires for it. For a PURE A32_0 config the window
bounds below are the entire guard: they are what stop an A32 mramAddress
entry from wandering into the SE-owned `atoc` band.

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

# System MRAM end (bench-confirmed; matches alif_flash.py's prior
# _SLOT0_REGION_END and the top of metadata's `storage` region).
MRAM_END = 0x80580000

# Size of the SE-owned `atoc` band at the very top of the App MRAM window
# (#1289) -- verbatim from both AEN presets' `memory_map:` `atoc` region,
# which is byte-identical on each part: metadata/e1m_modules/
# E1M-AEN801.yaml:274 and metadata/e1m_modules/E1M-AEN803.yaml:201, both
# `{ name: atoc, base: 0x80578000, size_kib: 32, ... }`. A generator-policy
# constant, not re-derived from that YAML at flash time -- same convention
# scripts/gen_zephyr_board.py's own `_AEN_ATOC_KIB` already uses, so this
# module stays stdlib-only (see module docstring).
_ATOC_BAND_KIB = 32

# Ceiling for the A32_0 slot0 window below: the base of that atoc band,
# i.e. MRAM_END minus the band's own size -- 0x80580000 - 32 KiB =
# 0x80578000, matching both presets' `atoc` `base:` verbatim. #1981
# initially used the raw MRAM_END here instead, which silently accepted
# an A32 mramAddress entry anywhere inside the SE-owned atoc band -- the
# exact vacuous-hardcode failure scripts/check_atoc_reservation.py's own
# docstring warns against.
_A32_0_CEILING = MRAM_END - _ATOC_BAND_KIB * 1024  # 0x80578000

# (base, size_bytes) per cpu_id -- see module docstring; mirrors
# metadata/e1m_modules/E1M-AEN801.yaml `memory_map:` he_slot0 / hp_slot0.
SLOT0_WINDOWS = {
    'M55_HE': (0x80010000, 2688 * 1024),
    'M55_HP': (0x802b0000, 2688 * 1024),
    # #1981: floor 0x80002000 is the BOOTLOAD (TF-A BL32) boot address
    # from the Alif APSS application-note A32 Linux config quoted in
    # #1981.  It is NOT verified on silicon: no bench run in this repo
    # has booted an A32 chain, and the AEN bench unit is down (#1883).
    # It also falls INSIDE the mcuboot region every AEN preset declares
    # (base 0x80000000, 64 KiB) -- consistent with an A32 chain the SE
    # boots without MCUboot, but do not read this floor as a measured
    # hardware fact.  Confirming it against a real SE boot table is
    # bench-owed.  Ceiling is _A32_0_CEILING (0x80578000, above), the base
    # of the SE-owned atoc band -- NOT the true per-build ceiling (the
    # signed ATOC's own start address moves with ATOC package size, see
    # SETOOLS' build/app-package-map.txt), but a fixed reservation band,
    # the same convention scripts/check_atoc_reservation.py enforces for
    # every other AEN partition table.
    'A32_0': (0x80002000, _A32_0_CEILING - 0x80002000),  # 5592 KiB
}

# Explicit M55 membership for the mutual-exclusivity check below -- NOT
# "every cpu_id that isn't A32_0". The Alif E8's A32 cluster is dual-core
# (a future `A32_1` key is plausible), and `SLOT0_WINDOWS` is not
# guaranteed to hold exactly {M55_HE, M55_HP, A32_0} forever; testing
# equality against this set keeps a future A32_1 entry from being
# mislabelled "an M55 mramAddress entry" and wrongly tripping the check.
M55_CPU_IDS = {'M55_HE', 'M55_HP'}


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

        mram_entries.append((name, base, cpu_id))

    seen: "dict[int, str]" = {}
    for name, base, _cpu_id in mram_entries:
        prior = seen.get(base)
        if prior is not None:
            raise AtocValidationError(
                f"ATOC entries '{prior}' and '{name}' both stage to the "
                f'same mramAddress 0x{base:x} -- flashing both would '
                'silently overwrite one image (#1069)')
        seen[base] = name

    # #1981: an A32_0 mramAddress entry's slot0-XIP span covers both M55
    # windows entirely (see module docstring), so an A32 config and an M55
    # mramAddress entry must never coexist -- that would stage an M55
    # slot0 image into memory the A32 chain is executing from. loadAddress
    # (ITCM) M55 stub entries never reach `mram_entries` above, so they
    # are unaffected by this check.
    a32_names = [n for n, _b, cid in mram_entries if cid == 'A32_0']
    m55_names = [n for n, _b, cid in mram_entries if cid in M55_CPU_IDS]
    if a32_names and m55_names:
        raise AtocValidationError(
            f"ATOC mixes an A32 mramAddress entry ({a32_names[0]!r}) with "
            f'an M55 mramAddress entry ({m55_names[0]!r}) -- an A32_0 '
            "chain's slot0-XIP span covers the whole M55 window range "
            '(#1981); no M55 mramAddress entry may coexist with an A32_0 '
            'one')


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
