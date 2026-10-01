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

Every call site above, plus the west runner, DOES call the second guard
here, ``validate_package_extent`` (#2234), right after ``app-gen-toc``: the
bench scripts through ``bench_atoc_extent_guard`` in
``scripts/bench/aen/bench-env.sh`` (which runs ``aen_atoc.py --package-map``),
the runner directly. It reads the package start SETOOLS records in
``build/app-package-map.txt`` and refuses a package that grew below the
``atoc`` band into the preset's customer ``storage`` region.

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

One remaining gap, NOT closed by this guard (see #1069's PR body):
  - The J-Link customer path (loadbin straight to slot0) has no ATOC
    assembly step at all; for that path the disjoint DTS windows ARE
    the guard (the link address itself moves per core).

(#2262 closed the other gap this docstring used to name: a sequential
single-core `west flash` writing a whole fresh TOC each run, invisible
to THIS window/overlap guard by the time the second `west flash` runs.
`scripts/west_commands/runners/alif_flash.py`'s ``do_run`` now reads the
resident TOC back over the SE-UART with `maintenance -opt gettoc`
*before* burning, and refuses the burn when that would silently delist a
foreign entry -- see the ATOC-replace guard functions below
(`compute_query_status`, `foreign_resident_entries`,
`decide_atoc_guard`), which mirror `bench_atoc_replace_guard` in
``scripts/bench/aen/bench-env.sh`` (alp-sdk#2025) so the two guards never
drift apart.)
"""

from __future__ import annotations

import json
import re
import sys
from dataclasses import dataclass, field
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
    # #1981.  It is NOT verified on silicon: Linux has not been booted
    # on the A32 cluster of any AEN module -- #1972 tracks first light.
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


# ---------------------------------------------------------------------------
# ATOC-replace guard (#2262): the query-side parser + verdict logic shared
# with `scripts/west_commands/runners/alif_flash.py`'s pre-burn guard.
#
# This is a PORT, not a rewrite, of `bench_atoc_replace_guard` in
# `scripts/bench/aen/bench-env.sh` -- every quirk below traces to a comment
# in that function, bench-verified on real AEN EVK captures (2026-09-07).
# `bench-env.sh` itself is NOT touched by this port (still bash, still the
# thing that runs on the bench today); a parity test
# (`tests/scripts/test_atoc_guard_parity.py`) feeds the SAME fixture
# transcripts through both implementations and asserts identical verdicts,
# so the two copies cannot silently drift apart. Unit tests for these
# functions on their own live in `tests/scripts/test_aen_atoc.py`. All
# functions here are pure (no subprocess, no file IO) -- the runner does
# the SE-UART query and handles these results.
# ---------------------------------------------------------------------------

# Strip any CSI (ANSI escape) sequence -- not just SGR colour (`...m`): a
# real SETOOLS capture also carries a cursor-show sequence (`\x1b[?25h`) and
# an erase-in-line (`\x1b[K`), and an unstripped one landing inside a Name
# cell would survive an SGR-only strip and read as a foreign entry (the
# false-alarm direction). Mirrors bench-env.sh's
# `sed -E 's/\x1b\[[0-9;?]*[a-zA-Z]//g'`.
_CSI_RE = re.compile(r'\x1b\[[0-9;?]*[a-zA-Z]')

# Bench-verified 2026-09-07: a real `getbanner` capture reads
# " SES A1 v1.110.0 Mar  4 2026 19:06:23" after ANSI stripping -- SETOOLS'
# own padding puts a LEADING SPACE on the line (not a terminal artifact), so
# a bare `^SES` anchor rejects every real banner. Tolerate leading
# whitespace, same as bench-env.sh's `^[[:space:]]*SES [^[:space:]]+ v[^[:space:]]+`
# -- POSIX `[[:space:]]` is space/tab/newline/CR/FF/VT; `\n` never appears
# mid-match here (this pattern is applied per already-split line via
# `re.MULTILINE`'s `^` anchor), so `[ \t\r\v\f]` covers the same set that
# can actually occur before `SES` on one line.
_SES_BANNER_RE = re.compile(r'^[ \t\r\v\f]*SES \S+ v\S+', re.MULTILINE)

# SETOOLS' exact "board is blank" message (`app-write-mram`'s own gettoc
# reply) -- matched case-insensitively as a WHOLE LINE, mirroring
# bench-env.sh's `grep -qix "no atoc found on target device."`. Anchoring to
# this exact text (not a bare substring) keeps an error line that merely
# CONTAINS "no atoc" (e.g. "no ATOC response from target") from decoding as
# a genuinely empty board.
#
# Deliberate deviation from the bash guard: bash's `grep` (POSIX BRE, no
# `-E`/`-F`) treats the trailing `.` as "any character", not a literal
# period, so bash would ALSO read e.g. "no atoc found on target deviceX"
# (any char in that position) as the empty-board line. Python's `==`
# below requires the literal `.` SETOOLS actually emits. This is a
# narrowing, fail-CLOSED divergence, not a bug to fix: the only way it can
# disagree with bash is a transcript bash would call "empty" that Python
# instead sends into the table parse (worst case, `unverified` or a
# legitimate-looking table with no rows -> still `unverified`, never a
# false "clear"). LOW-8 review (alp-sdk#2262): kept as-is, documented here
# rather than widened to match bash's wildcard.
_NO_ATOC_LINE = 'no atoc found on target device.'

# Second-round review (#2262): the changelog's own "not yet verified"
# section named this as the top open risk -- SETOOLS demonstrably pads the
# SES banner with a LEADING SPACE via its colour wrapping
# (`is_valid_ses_banner` above tolerates it), and the identical "No ATOC"
# reply is printed by the same tool through the same colour path, so it is
# very likely padded the same way. `is_no_atoc_found` strips this exact set
# of characters (ASCII space/tab/CR/VT/FF -- CR is already gone by the time
# it runs, kept here only for symmetry with `_SES_BANNER_RE`'s class) off
# each line before the exact-line compare, so a padded reply reads as the
# genuinely empty board it is instead of falling through to the table
# parse and landing on the wrong "unverified" (a false negative -- annoying
# and fail-closed, but not the direction that risks a delisting; still
# worth fixing so a truly blank board doesn't need --replace-atoc every
# time). Mirrored in bench-env.sh's `grep` pattern below.
_NO_ATOC_STRIP_CHARS = ' \t\r\v\f'

# Table rows look like "|   DEVICE |  CM0+  | 0x... | ... |" -- SETOOLS
# colours the WHOLE LINE, not just the cell text, so a real ANSI-stripped
# row keeps a LEADING SPACE before the pipe (bench-env.sh: a bare `/^\|/`
# anchor never matched a single real row). Rows may also carry a leading tab.
_ROW_RE = re.compile(r'^[ \t]*\|')
_DASH_ONLY_RE = re.compile(r'^-+$')

# DEVICE plus the two SE firmware banks (SERAM0/SERAM1 -- one marked
# "* SERAM0" as the currently-booted bank) are baseline SE state, never
# touched by `app-write-mram -p`: only a System Package update rewrites
# SERAM. Exempt ONLY when the CPU column is CM0+ (bench-env.sh cross-checks
# the CPU column too -- a same-named row on a DIFFERENT core is a
# coincidentally-named app entry, not SE firmware, and must still trip the
# guard).
_BASELINE_NAMES = frozenset({'DEVICE', 'SERAM0', 'SERAM1'})
_BASELINE_CPU = 'CM0+'

# A `+---+`-style box-drawing separator line (top, mid-table, or closing) in
# a real boxed `gettoc` capture, e.g. after ANSI/CR stripping:
# " +----------+--------+------------+...+----------+". Never matches a
# `_ROW_RE` pipe-row (it has no leading `|`), so it is invisible to
# `parse_resident_atoc_table`'s own loop -- `_is_table_structurally_complete`
# below scans for it separately.
_SEPARATOR_RE = re.compile(r'^[ \t]*\+[-+]*\+[ \t]*$')


def strip_csi(text: str) -> str:
    """Strip every ANSI CSI escape sequence from *text* (see `_CSI_RE`)."""
    return _CSI_RE.sub('', text)


def is_valid_ses_banner(banner_text: str) -> bool:
    """True if *banner_text* (raw `maintenance -opt getbanner` stdout)
    contains a line that parses as an SES banner after ANSI stripping."""
    return bool(_SES_BANNER_RE.search(strip_csi(banner_text)))


def is_no_atoc_found(gettoc_text: str) -> bool:
    """True if *gettoc_text* (raw `maintenance -opt gettoc` stdout) contains
    the exact, case-insensitive "No ATOC found on target device." line
    SETOOLS prints for a genuinely blank board, tolerating the leading/
    trailing whitespace SETOOLS' colour wrapping demonstrably adds to its
    other lines (see `_NO_ATOC_STRIP_CHARS`)."""
    stripped = strip_csi(gettoc_text).replace('\r', '')
    return any(
        line.strip(_NO_ATOC_STRIP_CHARS).casefold() == _NO_ATOC_LINE
        for line in stripped.split('\n'))


def _awk_field(fields: "list[str]", index: int) -> str:
    """awk's own semantics for a field past `NF`: `$n` for `n > NF` is the
    empty string, never an error/exception -- `fields` here is a 0-indexed
    Python `str.split('|')` result, so awk's `$2`/`$3` are `fields[1]`/
    `fields[2]`. A row with only ONE `|` (e.g. `" |  A32_APP"`, no closing
    `|` at all -- a real truncated-serial-read shape) still gives awk a
    non-empty `$2` ("A32_APP") and an empty `$3` ("") rather than being
    skipped outright; HIGH-1 review (alp-sdk#2262): an earlier version of
    this function `continue`d on `len(fields) < 3`, which SKIPPED that row
    entirely -- fail-OPEN relative to bash, since the resident entry it
    named then never reached `foreign_resident_entries` at all."""
    return fields[index] if index < len(fields) else ''


def parse_resident_atoc_table(gettoc_text: str) -> "list[tuple[str, str]]":
    """Parse *gettoc_text* (raw `maintenance -opt gettoc` stdout) into a
    list of `(name, cpu)` tuples, one per resident ATOC entry -- Name is
    field 2, CPU is field 3, both trimmed; the header ("Name") and the
    `+---+` separator rows are skipped. Mirrors bench-env.sh's awk table
    parse exactly, including its ANSI + CR handling, a short row (see
    `_awk_field`), and awk's OWN trim -- `gsub(/^[ \\t]+|[ \\t]+$/, "", ...)`
    strips only ASCII space/tab, never Python `str.strip()`'s full Unicode
    whitespace set (HIGH-1 review: a name cell of e.g. bare NBSP chars is
    non-empty, and thus a real -- if oddly named -- resident entry, to
    awk's gsub; `str.strip()` would fold it to `""` and DROP the row,
    again fail-open relative to bash)."""
    stripped = strip_csi(gettoc_text).replace('\r', '')
    resident: "list[tuple[str, str]]" = []
    for line in stripped.split('\n'):
        if not _ROW_RE.match(line):
            continue
        fields = line.split('|')
        name = _awk_field(fields, 1).strip(' \t')
        cpu = _awk_field(fields, 2).strip(' \t')
        if name and name != 'Name' and not _DASH_ONLY_RE.match(name):
            resident.append((name, cpu))
    return resident


def _scan_table_structure(gettoc_text: str):
    """Shared scan behind `_is_table_structurally_complete` and
    `table_has_structural_markers` -- one pass over *gettoc_text* that
    finds the header row's line index, the last data row's line index, and
    the last box-drawing separator's line index (each `None` if absent)."""
    stripped = strip_csi(gettoc_text).replace('\r', '')
    lines = stripped.split('\n')
    header_idx = None
    last_row_idx = None
    last_separator_idx = None
    for idx, line in enumerate(lines):
        if _SEPARATOR_RE.match(line):
            last_separator_idx = idx
            continue
        if not _ROW_RE.match(line):
            continue
        fields = line.split('|')
        name = _awk_field(fields, 1).strip(' \t')
        if not name or _DASH_ONLY_RE.match(name):
            continue
        if name == 'Name':
            header_idx = idx
            continue
        last_row_idx = idx
    return header_idx, last_row_idx, last_separator_idx


def _is_table_structurally_complete(gettoc_text: str) -> bool:
    """True only if *gettoc_text* carries BOTH the `| Name |` header row
    AND a closing `+---+` separator strictly AFTER the last parsed data
    row -- the BLOCKER finding of the second #2262 review round.

    MEASURED (alp-sdk#2538, E1M-AEN803 serial 2026W36-0009, SES A1 v1.110.0): a stalled SE-UART read makes `maintenance -opt gettoc` exit 0 with a truncated
    table (4/7/8 rows, or header only), because the host prints each row on arrival and
    the closing `+---+` line ONLY when the 0xa8 end packet arrives. This rule is
    therefore the PRIMARY defence against a stalled read. A closed port exits 1, and a
    complete table followed by `[ERROR] ... readSerial reporting disconnected` is caught
    only by the exit code, so a non-zero exit is still refused.

    A `gettoc` read that stops mid-download (a serial timeout after the SE
    has printed only its first few rows) can still exit rc=0 with a valid
    banner and >=1 real resident row -- e.g. just the two `DEVICE` rows plus
    `SERAM0`/`SERAM1` that precede any app entry in a real 9-row capture --
    which `compute_query_status`'s prior rule ("ok" once >=1 row parsed)
    accepted outright, silently burning over whatever app/A32-boot-chain
    entries the cut-off tail would have shown (the exact 2026-09-07 hardware
    loss this guard exists to close).

    THIRD review round: an earlier version of this function exempted a
    transcript carrying NEITHER a header nor a separator line at all,
    reasoning it was a "boxless/plain dump with nothing to check
    completeness against" -- that exemption REOPENED the same fail-open
    for any capture that happens to swallow both structural markers: noise
    eating the top separator+header while a timeout still cuts the tail
    (leaving only bare `DEVICE`/`SERAM0`/`SERAM1` rows, no markers at all),
    a different SETOOLS build using different box-drawing glyphs this
    parser has never seen, or SES boot-banner text interleaved into the
    transcript by a mid-query reset -- all of which parse >=1 row and carry
    no marker either, and all of which must refuse, not pass. A transcript
    with no recognisable table structure at all is not a shape this guard
    can vouch for; see `table_has_structural_markers` for the caller-facing
    helper that tells "torn table" (had a header, lost its footer) apart
    from "no recognised format at all" for refusal-message purposes only
    -- neither shape is ever `ok` here. Mirrors bench-env.sh's identical
    check in `bench_atoc_replace_guard`.
    """
    header_idx, last_row_idx, last_separator_idx = _scan_table_structure(gettoc_text)
    if header_idx is None or last_row_idx is None:
        return False
    return last_separator_idx is not None and last_separator_idx > last_row_idx


def table_has_structural_markers(gettoc_text: str) -> bool:
    """True if *gettoc_text* carries ANY boxed-table structural marker (the
    `| Name |` header row, or any `+---+` box-drawing separator line)
    anywhere in it -- used ONLY to choose which refusal message a caller
    (e.g. `alif_flash.py`'s `_run_atoc_guard`) shows for an `unverified`
    query that still parsed >=1 resident row: a table that has a header but
    lost its closing separator is a recognisable, torn capture ("confirm by
    hand, then --replace-atoc" is the right remedy); a table with NEITHER
    marker at all is not a shape this parser recognises as a gettoc table
    at all, and the right ask is "file the transcript", not "override the
    guard". Never used to decide the verdict itself -- that stays
    `_is_table_structurally_complete`, which requires BOTH markers."""
    header_idx, _last_row_idx, last_separator_idx = _scan_table_structure(gettoc_text)
    return header_idx is not None or last_separator_idx is not None


def compute_query_status(
    maintenance_available: bool,
    banner_text: "str | None",
    banner_rc: "int | None",
    gettoc_text: "str | None",
    gettoc_rc: "int | None",
) -> str:
    """Return "unverified" / "empty" / "ok" for a completed (or not even
    attempted) resident-ATOC query -- mirrors bench-env.sh's
    `query_status` computation:

    - no `maintenance` binary at all -> unverified (never assume safety);
    - a missing/garbled/non-zero-exit banner forces the query unverified
      REGARDLESS of what `gettoc` itself returned -- a gettoc read off the
      wrong serial device (the SE-UART vs. the app console) is not a safe
      verdict;
    - a non-zero `gettoc` exit (a closed port; a stall mid-packet) is never
      "ok"; a stall after a complete entry exits 0 and is caught by the
      closing-separator rule below, measured in alp-sdk#2538;
    - the exact "No ATOC found" line means a genuinely empty board;
    - otherwise "ok" only if at least one resident row parsed AND the table
      is structurally complete (see `_is_table_structurally_complete`) --
      rc=0 with neither the "No ATOC" line nor any parsed row, or a table
      torn off before its closing separator, stays unverified rather than
      silently defaulting to a pass.
    """
    if not maintenance_available:
        return 'unverified'
    banner_ok = banner_rc == 0 and is_valid_ses_banner(banner_text or '')
    effective_rc = gettoc_rc if banner_ok else 1
    if effective_rc != 0:
        return 'unverified'
    text = gettoc_text or ''
    if is_no_atoc_found(text):
        return 'empty'
    if parse_resident_atoc_table(text) and _is_table_structurally_complete(text):
        return 'ok'
    return 'unverified'


def foreign_resident_entries(
    resident: "list[tuple[str, str]]", allowed: "list[str] | set[str]"
) -> "list[str]":
    """Return the resident entry NAMES that are neither the DEVICE/SERAM0/
    SERAM1 baseline (on CM0+ only) nor in *allowed* -- the set of entries
    this run is itself about to (re)write. A leading "* " current-bank
    marker (e.g. "* SERAM0") is stripped ONLY for the baseline check; the
    membership check against *allowed* still uses the raw name, matching
    bench-env.sh exactly."""
    allowed_set = set(allowed)
    foreign: "list[str]" = []
    for name, cpu in resident:
        nbase = name[2:] if name.startswith('* ') else name
        if nbase in _BASELINE_NAMES and cpu == _BASELINE_CPU:
            continue
        if name not in allowed_set:
            foreign.append(name)
    return foreign


@dataclass(frozen=True)
class AtocGuardVerdict:
    """The pre-burn guard's decision. `status` is one of "clear" (nothing
    foreign, verified query), "empty" (verified query, genuinely blank
    board), "refused-foreign" (a foreign entry would be delisted),
    "refused-unverified" (the query could not be trusted), or "replaced"
    (--replace-atoc overrode a foreign or unverified finding). `refused`
    is True exactly when the caller must abort before burning."""
    status: str
    foreign: "list[str]" = field(default_factory=list)
    refused: bool = False


def decide_atoc_guard(
    query_status: str, foreign: "list[str]", replace_atoc: bool
) -> AtocGuardVerdict:
    """Turn a query outcome into the final verdict -- mirrors
    bench-env.sh's `bench_atoc_replace_guard` tail: an unverified query is
    checked BEFORE the foreign-entry check (an unverified query's `foreign`
    list is not trustworthy either way), and --replace-atoc overrides both,
    landing on "replaced" only when there was actually something to
    override (a clean, verified board with --replace-atoc still reports
    "clear"/"empty", never "replaced")."""
    if query_status == 'unverified':
        if replace_atoc:
            return AtocGuardVerdict(status='replaced')
        return AtocGuardVerdict(status='refused-unverified', refused=True)
    if foreign:
        if replace_atoc:
            return AtocGuardVerdict(status='replaced', foreign=list(foreign))
        return AtocGuardVerdict(
            status='refused-foreign', foreign=list(foreign), refused=True)
    return AtocGuardVerdict(status='empty' if query_status == 'empty' else 'clear')
_PACKAGE_START_RE = re.compile(r'APP Package Start Address:\s*(0x[0-9A-Fa-f]+)')


def package_start_from_map(text: str) -> int:
    """The package start address SETOOLS records in app-package-map.txt."""
    found = _PACKAGE_START_RE.findall(text)
    if not found:
        raise AtocValidationError(
            'app-package-map.txt has no "APP Package Start Address:" line')
    return int(found[-1], 16)


def validate_package_extent(start: int, allow_over_storage: bool = False) -> None:
    """Refuse a generated ATOC package that grows below the `atoc` band (#2234).

    SETOOLS top-anchors the package at MRAM_END and grows it downward by its
    size, and an ITCM load image (`loadAddress`) is stored INSIDE the package.
    A one-image Flow A package measured 89152 B, far past the 32 KiB band, so
    it reached down into the preset's customer `storage` region. A runtime
    that writes `storage` (NVS, littlefs, settings) or erase-storage.sh would
    then overwrite the boot image. *allow_over_storage* is the caller's
    explicit statement that the image never writes `storage`.
    """
    if start < _A32_0_CEILING and not allow_over_storage:
        raise AtocValidationError(
            f'the ATOC package starts at 0x{start:08X}, {_A32_0_CEILING - start} B below the '
            f'atoc band base 0x{_A32_0_CEILING:08X}: it overlaps the customer `storage` region, '
            'and anything that writes `storage` will overwrite the boot image (#2234). Shrink '
            'the load images, or pass --allow-over-storage (ALP_ATOC_ALLOW_OVER_STORAGE=1 in '
            'the bench scripts) only if this image never writes `storage`')


def main(argv: "list[str] | None" = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    try:
        if argv[:1] == ['--package-map'] and len(argv) in (2, 3):
            allow = argv[2:] == ['--allow-over-storage']
            if len(argv) == 3 and not allow:
                raise IndexError
            start = package_start_from_map(Path(argv[1]).read_text(encoding='utf-8'))
            validate_package_extent(start, allow)
            return 0
        if len(argv) != 1 or argv[0].startswith('--'):
            raise IndexError
        validate_atoc_config_file(argv[0])
    except IndexError:
        print('usage: aen_atoc.py <staged-atoc-config.json>\n'
              '       aen_atoc.py --package-map <app-package-map.txt> [--allow-over-storage]',
              file=sys.stderr)
        return 2
    except (AtocValidationError, OSError, json.JSONDecodeError) as exc:
        print(f'aen_atoc: REJECTED: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
