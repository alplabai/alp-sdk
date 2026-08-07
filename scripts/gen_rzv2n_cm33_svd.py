#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Generate a CMSIS-SVD 1.3 register-view file for the CM33 of the RZ/V2N
(`R9A09G056N`) from the vendored Renesas FSP headers (issue #1029 step 2).

**Why this exists.** Renesas publishes no CMSIS-SVD for the RZ/V2N. The
same register facts a vendor SVD would carry already live in the
`hal_renesas` west module's CMSIS-style FSP headers
(`.../Include/R9A09G056N/{iodefine.h,iodefines/*.h}` for the register
structs, `iobitmasks/*.h` for the `_Pos`/`_Msk` bitfield constants). This
script mechanically projects those headers into a `<device>` a debugger's
"peripheral view" can load, instead of a developer hand-copying addresses
out of the FSP source while bring-up debugging the CM33.

**Not committed, not a CI gate.** Every consumer of this SVD is mid-CM33-
debug and therefore already has a west workspace with `hal_renesas` checked
out -- the same workspace that built the firmware being debugged. Committing
the generated `.svd` would serve nobody and would force a second
`hal_renesas` pin into a CI workflow (the real pin resolves transitively
through Zephyr's own `west.yml`), which is a contract-drift trap. Run it
locally, on demand:

    python3 scripts/gen_rzv2n_cm33_svd.py --output /tmp/r9a09g056n_cm33.svd
    python3 scripts/gen_rzv2n_cm33_svd.py --check   # parse + validate, emit nothing

**The two independent vendor projections must agree.** `iodefines/` (C
register structs with real bitfield widths) and `iobitmasks/` (`_Pos`/`_Msk`
macros) are two hand-maintained views of the same register map. Every field
this script emits is cross-checked against the `iobitmasks/` side -- same
register, same bit position, same width -- and ANY disagreement or one-
sided field is a hard, non-zero-exit failure naming the peripheral,
register and field. A silently wrong or dropped bitfield would render a
WRONG value in a debugger's register view, which is worse than no register
view at all, so this check is unconditional, never opt-in, and never
downgraded to a warning -- with one explicit, narrow exception: the
`FIELD_CROSS_CHECK_SKIPS` table below names a small, closed set of 15
fields (against the real ~26-file corpus as of issue #1029 step 2; a
further 7 `iobitmasks/`-side macros with no `iodefine` counterpart at all
are named separately in `IOBITMASK_ORPHAN_SKIPS` -- the generator's
runtime stat line reports these two counts separately, as `field skip(s)`
and `orphan macro skip(s)`, never lumped into one number) where the two
vendor projections
themselves disagree in a way no parsing rule can resolve without
guessing. Those 15 fields are NOT silently emitted as if
cross-checked: each one either carries a `FIELD_POSITION_OVERRIDES` entry
(the position/width the OTHER evidence -- an inline vendor comment,
`iobitmasks/` itself -- agrees on, e.g. `spi_b`/`SPDCR2`/`TTRG`) or, absent
an override, emits with its raw struct-derived position/width AND a
`<description>NOT CROSS-VALIDATED: ...</description>` naming the exact
disagreement -- never a bare, indistinguishable-from-validated field. See
`FIELD_CROSS_CHECK_SKIPS`'s own comment for the full, itemised list and the
evidence behind each entry. A `FIELD_POSITION_OVERRIDES` entry is itself
re-corroborated against the iobitmasks side on every run (`cross_check_file`)
-- it is not a one-time trust decision baked in at review time; if the
vendor headers are later revised and the override's evidence no longer
holds, this is a hard fail naming the field, not a silently stale constant.

**Module-root discovery.** `--fsp-include-dir` takes precedence; otherwise
this resolves the west workspace topdir (walking up from this repo for a
`.west/` directory, the same convention `scripts/alp_cli/_workspace.py`
uses for the `alp` CLI) and looks under the west-module-standard path
Zephyr's own `west.yml` places `hal_renesas` at:

    <topdir>/modules/hal/renesas/drivers/rz/fsp/src/rzv/bsp/cmsis/Device/RENESAS/Include/R9A09G056N

If that directory (or the CPU-config header one level up,
`R9A09G056N.h`) is missing, this exits non-zero naming the exact path it
looked for -- it never emits a partial or empty SVD.

**Parsed constructs (verified against the real vendored headers before
writing this parser -- see `tests/scripts/test_gen_rzv2n_cm33_svd.py` for
the fixture that pins them):**

  * A register is a `union { <flat scalar/array>; struct { ...bitfields...
    } <name>_b[N]?; };` -- the flat member gives the register's own name,
    storage width (uint8_t/16/32_t only; a uint64_t member is an unhandled
    construct, hard-fail) and, for a register ARRAY, its element count. The
    struct tagged exactly `<name>_b` (dim suffix optional) is the field
    source; other struct arms in the same union (`_hword`, `_byte` in
    `ssi_iodefine.h` / `spibsc_iodefine.h`) are alternate access-width
    views of the SAME bits, not additional fields, and are ignored -- the
    `_b` arm is always the single semantic source of truth.
  * A register with no union at all (`__IOM uint32_t NXLA;`, no bitfield
    view) is a real register with zero fields -- both projections legally
    agree on "no fields", nothing to cross-check.
  * `RESERVEDn[SIZE]` (or scalar `RESERVEDn`) members are padding: they
    advance the address cursor and emit no `<register>`. One KNOWN
    exception: `canfd_iodefine.h` never declares a `CFDTXQOWSTS` register at
    all -- its 2 bytes fall inside a plain `RESERVED` span there, even
    though `canfd_iobitmask.h` independently defines `TXQ0OW`/`TXQ1OW`
    `_Pos`/`_Msk` macros for it (see `IOBITMASK_ORPHAN_SKIPS`). This script
    does not synthesise a register from the `iobitmasks/` side alone (that
    would be projecting from a SINGLE source, exactly the un-cross-checked
    emission this generator otherwise refuses to do) -- CFDTXQOWSTS is
    therefore knowingly, deliberately absent from the generated SVD, not
    silently dropped.
  * `__IOM R_<X>_Type <name>[N]?;` is a nested cluster reference -> SVD
    `<cluster>`, expanded recursively from the referenced type's own body
    (nested types are declared as separate top-level `typedef struct {...}
    R_<X>_Type;` blocks earlier in the same corpus, not lexically nested,
    so no recursive struct-body parsing is needed -- only recursive
    *expansion*, once every type is parsed into a flat table).
  * A type used as at least one `#define R_<INST>_BASE <addr>` /
    `#define R_<INST> ((R_<X>_Type *) R_<INST>_BASE)` pair is an
    independently-addressed peripheral (possibly with several instances
    sharing one type, e.g. `R_GPT0_Type` for GPT0..GPT17). A type with NO
    such pair is only ever reachable as a nested cluster member.

**Cross-check keying (why file-scoped, not path-scoped).** The vendor's own
`iobitmasks/` macro names are NOT a reliable function of the C nesting
path: a register nested inside `R_CANFD_CFDC_Type` gets the macro prefix
`R_CANFD_CFDC_...` (nesting-path-shaped), but a register belonging to the
*second, independently-based* peripheral type declared in a file
(`R_MHU_SWINT0_Type`, `R_RTC_RO_Type`) gets the macro prefix of the FILE'S
FIRST-declared type (`R_MHU0_...`, `R_RTC_...`) instead of its own type
name -- a real vendor-tooling inconsistency, verified by reading the
headers, not assumed. Reverse-engineering an exact prefix-construction rule
would silently break on the next such inconsistency. Instead, every field
this script parses out of `iodefines/<x>.h` is matched by (register name,
field name) SUFFIX against the `_Pos`/`_Msk` macros in the SAME FILE's
`iobitmasks/<x>.h` -- robust to whatever prefix scheme the vendor used, and
still fully rigorous: an ambiguous suffix match (two different registers
whose (regname, field) pair collides) or a one-sided field is a hard fail,
never a silent guess.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field as dc_field
from pathlib import Path
from xml.sax.saxutils import escape as xml_escape

REPO = Path(__file__).resolve().parents[1]

# --- Explicit skip-list -----------------------------------------------------
# Format: {(iodefine_filename_stem, register_name): "reason"}.  Checked
# explicitly by name below -- never a runtime `except: continue`.  A register
# lands here only when the TWO VENDOR PROJECTIONS THEMSELVES disagree in a way
# no parsing rule can reconcile without guessing which side is "right"; a
# parser limitation is a bug in this script, not a skip-list entry.
#
# Keyed by (iodefine file stem, register name, field name) -- as precise as
# the data allows, so a skip never hides an otherwise-clean sibling field in
# the same register (`i3c_b_iodefine.h` RSTCTL has 8 fields; only 1 of them
# disagrees). Every entry below was found by actually running this script
# against the real vendored headers (issue #1029 step 2) and reading both
# projections side by side -- none are guesses.
#
#   * gpio/ELC_DPTC/{PTC0,PTC1,PTC2,PTC3}: `gpio_iodefine.h` describes ONE
#     byte register "ELC_DPTC" with four named 1-bit fields. But
#     `gpio_iobitmask.h` has no `R_GPIO_ELC_DPTC_*` macros at all -- instead
#     four separate one-field register groups `R_GPIO_ELC_DPTC0_PTC` ..
#     `R_GPIO_ELC_DPTC3_PTC`. "One register, four fields" vs "four
#     registers, one field each": the two vendor files disagree on shape,
#     not just position.
#   * sci_b/RDR_BY/RDAT: iodefine says lsb=0 width=8 (the whole byte, as its
#     own inline layout implies: RDR_BY is RDR's 8-bit byte-access alias).
#     iobitmask's `R_SCI_B0_RDR_BY_RDAT` says lsb=28 width=1 -- IDENTICAL to
#     `R_SCI_B0_RDR_FER` two lines above it in the same file, strongly
#     suggesting a vendor copy-paste error in iobitmasks, not a real
#     alternate encoding. Neither side is safe to silently prefer.
#   * spi_b/SPDCR2/TTRG: iodefine's own struct computes lsb=10 (RTRG:4 at
#     bits[3:0], then an anonymous `: 6` pad occupying bits[9:4], so TTRG:4
#     starts at bit 10) -- but the SAME iodefine file's inline comment on
#     that very field says `[11..8]`, and iobitmask's
#     `R_SPI_B0_SPDCR2_TTRG` independently agrees with the comment
#     (lsb=8). All the evidence outside the raw C layout points at the
#     anonymous pad width being the bug (should be `: 4`, not `: 6`), but
#     this script parses what the struct actually says, not what looks
#     right, so it flags rather than silently repositions.
#   * xspi/BMCFGCH1/{WRMD,MWRCOMB,MWRSIZE,PREEN,CMBTIM} and
#     xspi/CMCTLCH1/{XIPENCODE,XIPEXCODE,XIPEN}: real registers/fields in
#     `xspi_iodefine.h`'s CS1 (channel 1) view, but `xspi_iobitmask.h` never
#     defines macros for either CH1 register at all (CH0's counterparts are
#     also absent, so this isn't a channel-numbering slip on one side) --
#     a genuine vendor documentation gap.
FIELD_CROSS_CHECK_SKIPS: dict[tuple[str, str, str], str] = {
    **{("gpio", "ELC_DPTC", f): "gpio_iodefine.h vs gpio_iobitmask.h register/field shape disagreement" for f in ("PTC0", "PTC1", "PTC2", "PTC3")},
    ("sci_b", "RDR_BY", "RDAT"): "sci_b RDR_BY/RDAT: iobitmask value looks copy-pasted from RDR/FER, see comment above",
    ("spi_b", "SPDCR2", "TTRG"): "spi_b SPDCR2/TTRG: iodefine struct layout disagrees with its own inline comment and with iobitmask",
    ("i3c_b", "RSTCTL", "RI3CRST"): "i3c_b RSTCTL: iodefine field 'RI3CRST' vs iobitmask field 'RI3C_BRST' (same bit0/width1, differently spelled)",
    **{
        ("xspi", "BMCFGCH1", f): "xspi CH1 register never got iobitmask macros (CH0's didn't either -- a vendor doc gap, not a channel-numbering slip)"
        for f in ("WRMD", "MWRCOMB", "MWRSIZE", "PREEN", "CMBTIM")
    },
    **{
        ("xspi", "CMCTLCH1", f): "xspi CH1 register never got iobitmask macros (CH0's didn't either -- a vendor doc gap, not a channel-numbering slip)"
        for f in ("XIPENCODE", "XIPEXCODE", "XIPEN")
    },
}

# Explicit (lsb, width) overrides for fields in FIELD_CROSS_CHECK_SKIPS where
# evidence OUTSIDE the raw iodefine struct layout (the SAME file's own inline
# comment, and/or the iobitmask side) agrees on a DIFFERENT position/width
# than what the struct arithmetic gives -- so emitting the struct-arithmetic
# value would ship a field known to be wrong, not merely unvalidated. Applied
# on the EMIT path only (RegisterMember.fields); the raw struct-derived
# (lsb, width) is still what field_records carries into cross_check_file, so
# the disagreement that justified the skip is still what gets skipped.
#
#   * spi_b/SPDCR2/TTRG: iodefine's struct computes lsb=10 (RTRG:4 at
#     bits[3:0], then an anonymous `: 6` pad occupying bits[9:4], so
#     TTRG:4 starts at bit 10 -- see the FIELD_CROSS_CHECK_SKIPS comment
#     above). TWO independent sources agree it should be lsb=8 width=4
#     instead: (1) the SAME struct's own inline comment on that field,
#     `/*!< [11..8] ... */`; (2) `spi_b_iobitmask.h`'s
#     `R_SPI_B0_SPDCR2_TTRG_Pos (8UL)` / `_Msk (0xF00UL)`. Confirmed against
#     the real vendored header (issue #1029 step 2): `spi_b_iodefine.h`'s
#     anonymous pad is `: 6` where `: 4` would make the struct self-
#     consistent with both other sources.
FIELD_POSITION_OVERRIDES: dict[tuple[str, str, str], tuple[int, int]] = {
    ("spi_b", "SPDCR2", "TTRG"): (8, 4),
}

# Orphan macros: present in iobitmasks/ with no reachable counterpart in
# iodefines/ at all (so there is no iodefine (regname, field) pair to key
# FIELD_CROSS_CHECK_SKIPS off of). Keyed by (file stem, EXACT macro base
# name, without _Pos/_Msk) -- exact, not an unanchored substring: an
# unanchored substring can match more macros than the entry's own reasoning
# accounts for, and gives no signal when the corpus grows a new macro that
# happens to contain the same fragment. Every entry here pairs 1:1 with the
# FIELD_CROSS_CHECK_SKIPS reasoning above except CFDTXQOWSTS, which has no
# iodefine-side struct member at all (`canfd_iodefine.h` never declares a
# CFDTXQOWSTS register -- those 2 bytes fall inside a RESERVED span instead).
#
# spi_b/SPDCR2/TTRG and sci_b/RDR_BY/RDAT are deliberately ABSENT here: their
# iobitmask macros ARE found by `_find_macro` (same (regname, field) suffix
# match as any other field -- the field-level skip is what suppresses the
# resulting mismatch), so they are already marked `consumed` by the normal
# field-lookup path in `cross_check_file` and never reach the orphan loop at
# all. An orphan-skip entry for either is unreachable dead code; the
# "unused skip-list entry" strictness in generate() proved this (issue #1029
# step 2 review) by reporting both entries as never matched.
IOBITMASK_ORPHAN_SKIPS: dict[tuple[str, str], str] = {
    ("canfd", "R_CANFD_CFDTXQOWSTS_TXQ0OW"): "canfd_iodefine.h has no CFDTXQOWSTS register at all (it's RESERVED padding there)",
    ("canfd", "R_CANFD_CFDTXQOWSTS_TXQ1OW"): "canfd_iodefine.h has no CFDTXQOWSTS register at all (it's RESERVED padding there)",
    ("gpio", "R_GPIO_ELC_DPTC0_PTC"): "see FIELD_CROSS_CHECK_SKIPS gpio/ELC_DPTC",
    ("gpio", "R_GPIO_ELC_DPTC1_PTC"): "see FIELD_CROSS_CHECK_SKIPS gpio/ELC_DPTC",
    ("gpio", "R_GPIO_ELC_DPTC2_PTC"): "see FIELD_CROSS_CHECK_SKIPS gpio/ELC_DPTC",
    ("gpio", "R_GPIO_ELC_DPTC3_PTC"): "see FIELD_CROSS_CHECK_SKIPS gpio/ELC_DPTC",
    ("i3c_b", "R_I3C_B_RSTCTL_RI3C_BRST"): "see FIELD_CROSS_CHECK_SKIPS i3c_b/RSTCTL/RI3CRST",
}

# Types whose vendor `/*!< Size = N (0xNN) */` trailing comment (on the
# type's own closing brace) is skipped by the size-hint cross-check in
# expand_type() -- a SEPARATE, purely opportunistic sanity check (not
# required by the iodefine/iobitmask field cross-check above). Only 9 of
# the real corpus's 43 parsed types carry such a comment at all (verified
# #1029 step 2); for the other 34, expand_type() has nothing to compare
# against and is a silent no-op -- this check catches a stale Size hint,
# it does NOT independently corroborate the struct walk for most types, so
# a clean run here is not evidence a type's real extent is right, only
# that a hint (where one exists) doesn't contradict it.
#
#   * R_CANFD_Type: computes to 0x1C000 bytes by walking the struct, but the
#     vendor's own comment says 0x14000. Every individual member's offset
#     independently matches this SAME file's own per-register inline
#     `/*!< (@ 0x......) ... */` address comments exactly, all the way
#     through and including CFDTM's base address (0x10000) -- so the struct
#     WALK is correct; the outer type's one summary comment (0x14000) is
#     simply stale relative to CFDTM's real declared extent
#     (`CFDTM[384]` of 128-byte `R_CANFD_CFDTM_Type` = 0xC000, pushing the
#     end past 0x10000 + 0xC000 = 0x1C000). Confirmed by direct inspection,
#     not assumed.
SIZE_HINT_SKIPS: set[str] = {"R_CANFD_Type"}

# Peripherals (by top-level addressed instance name) this script cannot
# express at all and skips emitting entirely. Kept as its own constant,
# separate from FIELD_CROSS_CHECK_SKIPS, because it is a strictly bigger
# hammer -- drops a whole peripheral's register view, not one register.
# Empty is the goal.
PERIPHERAL_SKIPS: dict[str, str] = {}

# Verified by listing iodefines/*.h against the real hal_renesas checkout
# (issue #1029 step 2). Used only to gate the skip-list "unused entry"
# strictness above -- a run against fewer files (a test fixture) can't be
# expected to exercise every real-corpus skip entry.
REAL_IODEFINE_FILE_COUNT = 26

SIZE_OF = {"uint8_t": 1, "uint16_t": 2, "uint32_t": 4}
ACCESS_OF = {"IM": "read-only", "OM": "write-only", "IOM": "read-write"}

COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
SIZE_HINT_RE = re.compile(r"\}\s*(R_[A-Za-z0-9_]+_Type)\s*;\s*/\*!<\s*Size\s*=\s*(\d+)")
BASE_RE = re.compile(r"^#define\s+(R_[A-Za-z0-9_]+)_BASE\s+(0x[0-9A-Fa-f]+)$")
# The pointer macro's own instance name usually matches its `_BASE` macro
# (`R_GPT0` -> `R_GPT0_BASE`), but NOT always: `poeg_iodefine.h` names its
# eight instances `R_POEGA`..`R_POEGH` while their `_BASE` macros are
# `R_POEG0A_BASE`..`R_POEG1D_BASE` -- a real, verified vendor naming split
# (issue #1029 step 2; silently requiring the two names to match dropped
# all 8 POEG instances with no error until this was found by comparing the
# emitted peripheral count against a manual count of `_BASE` macros). The
# base-macro reference is therefore captured independently, group 3, not
# backreferenced from the instance name in group 1.
PTR_RE = re.compile(
    r"^#define\s+(R_[A-Za-z0-9_]+)\s+\(\((R_[A-Za-z0-9_]+_Type)\s*\*\)\s*(R_[A-Za-z0-9_]+_BASE)\)$"
)
PLAIN_RE = re.compile(
    r"^__(IM|OM|IOM)\s+(uint8_t|uint16_t|uint32_t|uint64_t|R_[A-Za-z0-9_]+_Type)"
    r"\s+([A-Za-z_][A-Za-z0-9_]*)(?:\[(\d+)\])?;$"
)
NAMED_BIT_RE = re.compile(
    r"^__(IM|OM|IOM)\s+(uint8_t|uint16_t|uint32_t|uint64_t)"
    r"\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(\d+);$"
)
ANON_BIT_RE = re.compile(r"^(uint8_t|uint16_t|uint32_t|uint64_t)\s*:\s*(\d+);$")
CLOSE_TAG_RE = re.compile(r"^\}\s*([A-Za-z_][A-Za-z0-9_]*)(?:\[(\d+)\])?;$")
CLOSE_TYPE_RE = re.compile(r"^\}\s*(R_[A-Za-z0-9_]+_Type)\s*;$")
CPU_DEFINE_RE = re.compile(r"^#define\s+(__[A-Za-z0-9_]+)\s+(\S+)")


class SvdGenError(RuntimeError):
    """A construct this script cannot express, or a real cross-check
    disagreement -- always carries a message naming exactly what and where."""


def fail(msg: str) -> "NoReturn":  # noqa: F821 - typing string is fine at runtime
    raise SvdGenError(msg)


# --- Parsed-member shapes ---------------------------------------------------


@dataclass
class Padding:
    nbytes: int


@dataclass
class RegisterMember:
    name: str
    elem_type: str  # uint8_t / uint16_t / uint32_t
    count: int | None  # None = scalar register, else array length
    access: str  # IM / OM / IOM
    # (name, lsb, width, note) -- note is None for a normally cross-checked
    # field, else a human-readable string emitted as the field's
    # <description> (see parse_union: an overridden or knowingly-unvalidated
    # skip-list field always carries a non-None note, never emits silently).
    fields: list[tuple[str, int, int, str | None]] = dc_field(default_factory=list)


@dataclass
class ClusterMember:
    name: str
    type_name: str
    count: int | None


@dataclass
class Overlay:
    """One or more registers that all start at the SAME address offset --
    a union whose arms are themselves `union { <reg>; struct {...} <reg>_b; }`
    blocks (a real, if uncommon, vendor pattern: `crc_iodefine.h`'s CRCDIR /
    CRCDIR_BY, `rtc_iodefine.h`'s many BCD/binary alias pairs). Consumes the
    MAX of its members' sizes once, not the sum."""

    regs: list["RegisterMember | ClusterMember"]


Member = Padding | RegisterMember | ClusterMember | Overlay


# --- Low-level line parsing --------------------------------------------------


def clean_lines(text: str) -> list[str]:
    text = COMMENT_RE.sub("", text)
    return [ln.strip() for ln in text.splitlines() if ln.strip()]


def parse_member_decl(line: str, ctx: str) -> tuple[str, str, str, int | None]:
    m = PLAIN_RE.match(line)
    if not m:
        fail(f"{ctx}: unrecognised member declaration: {line!r}")
    access, typ, name, count = m.groups()
    if typ == "uint64_t":
        fail(f"{ctx}: member {name!r} uses uint64_t storage -- unsupported width")
    return access, typ, name, (int(count) if count else None)


def classify_plain(access: str, typ: str, name: str, count: int | None, ctx: str) -> Member:
    if typ.startswith("R_") and typ.endswith("_Type"):
        return ClusterMember(name=name, type_name=typ, count=count)
    if re.match(r"^RESERVED\d*$", name):
        return Padding(nbytes=SIZE_OF[typ] * (count or 1))
    # A plain register with no bitfield view at all (e.g. DMAC's SA/DA/TB).
    return RegisterMember(name=name, elem_type=typ, count=count, access=access, fields=[])


def parse_bitfield_line(line: str, ctx: str) -> tuple[str | None, int] | None:
    """Parse one line inside a `struct {...} <tag>;` union arm.

    Returns (field_name_or_None, width) for a real bitfield line. Returns
    None for a plain (non-bitfield) member line -- `spibsc_iodefine.h` /
    `ssi_iodefine.h`'s `_hword`/`_byte` alternate-access-width arms hold
    plain `uint16_t L; uint16_t H;` / `uint8_t LL; ...` sub-members, not
    bitfields; those arms are never the one selected as a register's field
    source (only the sibling tagged exactly `<name>_b` is, see
    `parse_union`), so their content only needs to parse cleanly, not
    contribute fields.
    """
    m = NAMED_BIT_RE.match(line)
    if m:
        _, typ, name, width = m.groups()
        if typ == "uint64_t":
            fail(f"{ctx}: bitfield {name!r} uses uint64_t storage -- unsupported width")
        return name, int(width)
    m = ANON_BIT_RE.match(line)
    if m:
        typ, width = m.groups()
        if typ == "uint64_t":
            fail(f"{ctx}: anonymous bitfield uses uint64_t storage -- unsupported width")
        return None, int(width)
    if PLAIN_RE.match(line):
        return None
    fail(f"{ctx}: unrecognised line inside a union arm: {line!r}")


def _is_reserved_member_line(line: str) -> bool:
    m = PLAIN_RE.match(line)
    return bool(m) and re.match(r"^RESERVED\d*$", m.group(3)) is not None


def parse_group_struct_body(
    lines: list[str], i: int, ctx: str, field_records: list[tuple[str, str, int, int]]
) -> tuple[list[RegisterMember], int]:
    """Parse the body of an ANONYMOUS grouping `struct { ... };` arm (no
    tag on its own close -- see `parse_union`'s 'union mixes...' note).
    Only two shapes are legal inside one: a nested `union {...}` (the real
    overlaid register(s), recursed into) or `RESERVED[n];` padding, which
    is discarded -- it only pads that ONE arm's own C struct size to match
    a sibling arm, and never affects the enclosing union's own offset.
    Anything else is an unhandled construct, not a guess.
    """
    nested: list[RegisterMember] = []
    while not lines[i].startswith("}"):
        if lines[i] == "union":
            if lines[i + 1] != "{":
                fail(f"{ctx}: 'union' not followed by '{{': {lines[i + 1]!r}")
            sub_regs, i = parse_union(lines, i + 2, ctx, field_records)
            nested.extend(sub_regs)
            continue
        if _is_reserved_member_line(lines[i]):
            i += 1
            continue
        fail(f"{ctx}: anonymous grouping struct has an unhandled member: {lines[i]!r}")
    return nested, i


def parse_union(
    lines: list[str], i: int, ctx: str, field_records: list[tuple[str, str, int, int]]
) -> tuple[list[RegisterMember], int]:
    """Parse a `union { ... };` body (the opening 'union' and '{' already
    consumed by the caller; *i* points at the first line inside).

    Ordinarily a union has exactly one flat scalar/array member (the
    register's own storage) plus zero or more `struct {...} <tag>;` arms,
    of which only the one tagged `<name>_b` carries real fields (see the
    `_hword`/`_byte` alternate-width-view note in the module docstring).
    But some unions instead contain one or more NESTED `union {...}`
    blocks -- several differently-named, differently-sized registers
    genuinely overlaid at the same address (`crc_iodefine.h` CRCDIR /
    CRCDIR_BY, plenty of `rtc_iodefine.h` BCD-alias pairs). Those two
    shapes are mutually exclusive; a union mixing them is an unhandled
    construct, not a guess.
    """
    primary: tuple[str, str, str, int | None] | None = None
    arms: dict[str, list[tuple[str | None, int]]] = {}
    nested: list[RegisterMember] = []
    while lines[i] != "};":
        if lines[i] == "union":
            if lines[i + 1] != "{":
                fail(f"{ctx}: 'union' not followed by '{{': {lines[i + 1]!r}")
            sub_regs, i = parse_union(lines, i + 2, ctx, field_records)
            nested.extend(sub_regs)
            continue
        if lines[i] == "struct":
            if lines[i + 1] != "{":
                fail(f"{ctx}: 'struct' not followed by '{{': {lines[i + 1]!r}")
            body_start = i + 2
            if lines[body_start] == "union" or _is_reserved_member_line(lines[body_start]):
                # An ANONYMOUS grouping struct (bare '};' close, no tag) --
                # `i3c_b_iodefine.h`'s NTDTBP0/NTDTBP0_BY: two struct arms,
                # each just a wrapper around one nested `union {...}` (the
                # real overlaid register) plus, in one arm, RESERVED padding
                # that only pads that arm's own C struct size to match its
                # sibling -- irrelevant to SVD offsets, since both arms
                # start at the SAME address (they're inside the outer
                # union) and only the max size across arms matters.
                nested_here, j = parse_group_struct_body(lines, body_start, ctx, field_records)
                if lines[j] != "};":
                    fail(f"{ctx}: expected bare '}};' closing an anonymous grouping struct, got {lines[j]!r}")
                nested.extend(nested_here)
                i = j + 1
                continue
            i = body_start
            arm_fields: list[tuple[str | None, int]] = []
            while not lines[i].startswith("}"):
                entry = parse_bitfield_line(lines[i], ctx)
                if entry is not None:
                    arm_fields.append(entry)
                i += 1
            m = CLOSE_TAG_RE.match(lines[i])
            if not m:
                fail(f"{ctx}: unrecognised bitfield-struct close: {lines[i]!r}")
            tag = m.group(1)
            i += 1
            arms[tag] = arm_fields
            continue
        if primary is not None:
            fail(f"{ctx}: union has more than one flat storage member (near {lines[i]!r})")
        primary = parse_member_decl(lines[i], ctx)
        i += 1
    i += 1  # consume the union's own '};'
    if nested:
        if primary is not None or arms:
            fail(f"{ctx}: union mixes nested 'union {{}}' arms with a flat/struct arm")
        return nested, i
    if primary is None:
        fail(f"{ctx}: union has no flat storage member")
    access, typ, name, count = primary
    if typ not in SIZE_OF:
        fail(f"{ctx}: union primary member {name!r} has unsupported type {typ!r}")
    chosen = arms.get(f"{name}_b", [])
    stem = ctx[: -len("_iodefine.h")] if ctx.endswith("_iodefine.h") else None
    fields: list[tuple[str, int, int, str | None]] = []
    lsb = 0
    for fname, width in chosen:
        if fname is not None:
            # field_records always carries the RAW struct-derived (lsb,
            # width) -- cross_check_file reads this, and it must see
            # exactly what the iodefine struct says to detect (or
            # knowingly skip) a real disagreement with iobitmasks.
            field_records.append((name, fname, lsb, width))
            skip_key = (stem, name, fname) if stem else None
            override = FIELD_POSITION_OVERRIDES.get(skip_key) if skip_key else None
            if override is not None:
                emit_lsb, emit_width = override
                reason = FIELD_CROSS_CHECK_SKIPS.get(skip_key, "position/width overridden")
                note = f"POSITION OVERRIDDEN (struct layout says lsb={lsb} width={width}): {reason}"
            elif skip_key is not None and skip_key in FIELD_CROSS_CHECK_SKIPS:
                emit_lsb, emit_width = lsb, width
                note = f"NOT CROSS-VALIDATED: {FIELD_CROSS_CHECK_SKIPS[skip_key]}"
            else:
                emit_lsb, emit_width = lsb, width
                note = None
            fields.append((fname, emit_lsb, emit_width, note))
        lsb += width
    total_bits = SIZE_OF[typ] * 8
    if chosen and lsb != total_bits:
        fail(
            f"{ctx}: register {name!r} bitfield widths sum to {lsb} bits, "
            f"expected {total_bits} for {typ}"
        )
    return [RegisterMember(name=name, elem_type=typ, count=count, access=access, fields=fields)], i


def parse_struct_body(
    lines: list[str], i: int, ctx: str, field_records: list[tuple[str, str, int, int]]
) -> tuple[list[Member], int]:
    members: list[Member] = []
    while True:
        line = lines[i]
        if line.startswith("}"):
            return members, i
        if line == "union":
            if lines[i + 1] != "{":
                fail(f"{ctx}: 'union' not followed by '{{': {lines[i + 1]!r}")
            regs, i = parse_union(lines, i + 2, ctx, field_records)
            members.append(regs[0] if len(regs) == 1 else Overlay(regs=regs))
            continue
        access, typ, name, count = parse_member_decl(line, ctx)
        members.append(classify_plain(access, typ, name, count, ctx))
        i += 1


@dataclass
class ParsedFile:
    types: dict[str, list[Member]]
    field_records: list[tuple[str, str, int, int]]  # (regname, field, lsb, width)
    instances: list[tuple[str, str, int]]  # (inst_name, type_name, base_addr)
    size_hints: dict[str, int]  # type_name -> vendor-documented size in bytes


def parse_iodefine_file(path: Path) -> ParsedFile:
    ctx = path.name
    raw = path.read_text(encoding="utf-8")
    size_hints = {m.group(1): int(m.group(2)) for m in SIZE_HINT_RE.finditer(raw)}
    lines = clean_lines(raw)
    types: dict[str, list[Member]] = {}
    field_records: list[tuple[str, str, int, int]] = []
    bases: dict[str, int] = {}
    ptrs: dict[str, tuple[str, str]] = {}  # inst -> (type_name, base_macro_name)
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        if line == "typedef struct":
            if lines[i + 1] != "{":
                fail(f"{ctx}: 'typedef struct' not followed by '{{': {lines[i + 1]!r}")
            members, close_i = parse_struct_body(lines, i + 2, ctx, field_records)
            m = CLOSE_TYPE_RE.match(lines[close_i])
            if not m:
                fail(f"{ctx}: expected '}} R_..._Type;' closing a typedef, got {lines[close_i]!r}")
            type_name = m.group(1)
            if type_name in types:
                fail(f"{ctx}: duplicate type definition {type_name!r}")
            types[type_name] = members
            i = close_i + 1
            continue
        m = BASE_RE.match(line)
        if m:
            bases[m.group(1)] = int(m.group(2), 16)
            i += 1
            continue
        m = PTR_RE.match(line)
        if m:
            ptrs[m.group(1)] = (m.group(2), m.group(3))
            i += 1
            continue
        i += 1
    instances: list[tuple[str, str, int]] = []
    for inst, (typ, base_macro) in ptrs.items():
        base_key = base_macro[: -len("_BASE")]
        if base_key not in bases:
            fail(f"{ctx}: {inst} points at {base_macro}, but no matching '#define {base_macro} 0x...' exists")
        instances.append((inst, typ, bases[base_key]))
    # The reverse direction of the check just above: a `#define R_X_BASE
    # 0x...` with no pointer macro that resolves to it at all (POEG_RE
    # match fails, e.g. a base-macro naming scheme PTR_RE doesn't
    # recognise) drops that peripheral SILENTLY -- no error, just one
    # fewer `<peripheral>` in the output. This is exactly the bug class
    # that lost all 8 POEG instances before PTR_RE's group-3 base-macro
    # capture was added (see PTR_RE's own comment): a `_BASE` macro with
    # no owner.
    orphan_bases = set(bases) - {base_macro[: -len("_BASE")] for _, base_macro in ptrs.values()}
    if orphan_bases:
        fail(f"{ctx}: '_BASE' macro(s) with no pointer macro resolving to them: {sorted(orphan_bases)}")
    return ParsedFile(types=types, field_records=field_records, instances=instances, size_hints=size_hints)


def parse_cpu_header(path: Path) -> dict[str, int]:
    text = path.read_text(encoding="utf-8")
    m = re.search(r"#if\s+defined\(BSP_SUPPORT_CORE_CM33\)(.*?)(?:#elif|#endif)", text, re.S)
    if not m:
        fail(f"{path}: no '#if defined(BSP_SUPPORT_CORE_CM33)' block found")
    values: dict[str, str] = {}
    for line in m.group(1).splitlines():
        dm = CPU_DEFINE_RE.match(line.strip())
        if dm:
            values[dm.group(1)] = dm.group(2)
    required = ["__CM33_REV", "__NVIC_PRIO_BITS", "__MPU_PRESENT", "__FPU_PRESENT", "__DSP_PRESENT"]
    missing = [r for r in required if r not in values]
    if missing:
        fail(f"{path}: BSP_SUPPORT_CORE_CM33 block missing {missing}")

    def as_int(raw: str) -> int:
        return int(raw.rstrip("UuLl"), 0)

    return {k: as_int(values[k]) for k in required}


# --- Cross-checking against iobitmasks/ -------------------------------------

POS_RE = re.compile(r"^#define\s+(R_[A-Za-z0-9_]+)_Pos\s+\((\d+)UL\)")
MSK_RE = re.compile(r"^#define\s+(R_[A-Za-z0-9_]+)_Msk\s+\((0x[0-9A-Fa-f]+)UL\)")


def parse_iobitmask_macros(path: Path) -> dict[str, tuple[int, int]]:
    """macro base name (without _Pos/_Msk) -> (pos, msk)."""
    pos: dict[str, int] = {}
    msk: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        m = POS_RE.match(line)
        if m:
            pos[m.group(1)] = int(m.group(2))
            continue
        m = MSK_RE.match(line)
        if m:
            msk[m.group(1)] = int(m.group(2), 16)
    only_pos = set(pos) - set(msk)
    only_msk = set(msk) - set(pos)
    if only_pos or only_msk:
        fail(f"{path.name}: _Pos/_Msk macros without a matching pair: {only_pos | only_msk}")
    return {k: (pos[k], msk[k]) for k in pos}


def mask_width(msk: int, macro: str, ctx: str) -> int:
    if msk == 0:
        fail(f"{ctx}: {macro} has a zero mask")
    pos = 0
    p = msk
    while not (p & 1):
        pos += 1
        p >>= 1
    width = 0
    while p & 1:
        width += 1
        p >>= 1
    if (((1 << width) - 1) << pos) != msk:
        fail(f"{ctx}: {macro} mask 0x{msk:X} is not a single contiguous run of bits")
    return width


def _find_macro(bitmasks: dict[str, tuple[int, int]], regname: str, fname: str) -> list[str]:
    """Longest-suffix candidates for `_<regname>_<fname>` among bitmask macro
    base names. Tries *regname* verbatim first; if that finds nothing, retries
    with its leading `<TOKEN>_` stripped once -- some files repeat that
    leading token in the register's own iodefine name but the vendor's
    iobitmask macro does not (e.g. `tsu_b_iodefine.h` register `TSU_SSUSR`
    vs macro `R_TSU_B0_SSUSR_...`, no repeated `TSU_`). Only ever a fallback
    for an otherwise-empty result, so it can rescue a lookup that would
    otherwise fail; it can never override an exact match that already
    succeeded, so it cannot introduce ambiguity into an already-working case.
    """
    suffix = f"_{regname}_{fname}"
    candidates = sorted((mb for mb in bitmasks if mb.endswith(suffix)), key=len, reverse=True)
    if candidates or "_" not in regname:
        return candidates
    stripped = regname.split("_", 1)[1]
    suffix2 = f"_{stripped}_{fname}"
    return sorted((mb for mb in bitmasks if mb.endswith(suffix2)), key=len, reverse=True)


def cross_check_file(
    stem: str, field_records: list[tuple[str, str, int, int]], bitmasks: dict[str, tuple[int, int]]
) -> tuple[list[str], set[tuple[str, str, str]], set[tuple[str, str]], set[tuple[str, str, str]]]:
    """Returns (problems, used_field_skips, used_orphan_skips, used_position_overrides).

    A FIELD_CROSS_CHECK_SKIPS entry is recorded as "used" ONLY when it
    actually suppressed a would-be `problems.append` -- the lookup always
    runs first, exactly as for a non-skipped field, never short-circuited
    by skip-list membership. A skip whose disagreement has since stopped
    reproducing (the corpus now agrees, or the entry never matched a real
    field at all) is therefore caught as an "unused skip-list entry" by the
    strictness check in generate(), instead of silently counting as "used"
    forever because its key was merely seen (issue #1029 step 2 review:
    this exact gap is how 32 dead dmac_b/DSTAT_* entries went undetected).

    A FIELD_POSITION_OVERRIDES entry is re-corroborated here on every run,
    not merely trusted because a comment once justified it: if its key's
    iobitmask side no longer resolves to exactly one macro, or that macro's
    own (pos, width) no longer equals the override's value, this is a HARD
    FAIL naming the field -- an override that stops agreeing with its own
    cited evidence is exactly the "wrong bit silently ships" defect this
    whole generator exists to prevent, and a stale override is no different
    from a stale skip in that respect (issue #1029 step 2 second review).
    """
    problems: list[str] = []
    consumed: set[str] = set()
    used_field_skips: set[tuple[str, str, str]] = set()
    used_orphan_skips: set[tuple[str, str]] = set()
    used_position_overrides: set[tuple[str, str, str]] = set()
    for regname, fname, lsb, width in field_records:
        skip_key = (stem, regname, fname)
        is_skip = skip_key in FIELD_CROSS_CHECK_SKIPS
        override = FIELD_POSITION_OVERRIDES.get(skip_key)
        candidates = _find_macro(bitmasks, regname, fname)
        if override is not None:
            used_position_overrides.add(skip_key)
            if len(candidates) != 1:
                fail(
                    f"{stem}: register {regname!r} field {fname!r}: FIELD_POSITION_OVERRIDES "
                    f"entry can no longer be re-corroborated -- iobitmask match is "
                    f"{candidates!r}, expected exactly one"
                )
            macro = candidates[0]
            pos, msk = bitmasks[macro]
            bit_width = mask_width(msk, macro, stem)
            if (pos, bit_width) != override:
                fail(
                    f"{stem}: register {regname!r} field {fname!r}: FIELD_POSITION_OVERRIDES "
                    f"says lsb={override[0]} width={override[1]}, but iobitmask {macro} now "
                    f"says lsb={pos} width={bit_width} -- the override no longer agrees with "
                    f"its own cited evidence, re-derive or drop it"
                )
        problem: str | None = None
        if not candidates:
            problem = (
                f"{stem}: register {regname!r} field {fname!r}: present in iodefine, "
                f"no matching _Pos/_Msk macro in iobitmasks"
            )
        elif len(candidates) > 1:
            # ANY ambiguity is a hard stop, not just a same-length tie --
            # "longest wins" is exactly as much of a guess the moment two
            # candidates differ in length, it just happens the real corpus
            # never hits that branch (0 of 9188 lookups return >1 candidate
            # at all, verified #1029 step 2 review; this widening is a
            # no-op against the real corpus, not a behaviour change).
            problem = (
                f"{stem}: register {regname!r} field {fname!r}: ambiguous iobitmask "
                f"match {candidates!r}"
            )
        else:
            macro = candidates[0]
            consumed.add(macro)
            pos, msk = bitmasks[macro]
            bit_width = mask_width(msk, macro, stem)
            if pos != lsb or bit_width != width:
                problem = (
                    f"{stem}: register {regname!r} field {fname!r}: iodefine says "
                    f"lsb={lsb} width={width}, iobitmask {macro} says lsb={pos} width={bit_width}"
                )
        if problem is None:
            continue
        if is_skip:
            used_field_skips.add(skip_key)
        else:
            problems.append(problem)
    for macro in bitmasks:
        if macro in consumed:
            continue
        skip_key = (stem, macro)
        if skip_key in IOBITMASK_ORPHAN_SKIPS:
            used_orphan_skips.add(skip_key)
            continue
        problems.append(f"{stem}: iobitmask macro {macro} has no matching field in iodefine")
    return problems, used_field_skips, used_orphan_skips, used_position_overrides


# --- Expansion into an address-mapped register/cluster tree ------------------


@dataclass
class SvdRegister:
    name: str
    offset: int
    elem_bits: int
    access: str
    fields: list[tuple[str, int, int, str | None]]
    dim: int | None
    alternate_register: str | None = None


@dataclass
class SvdCluster:
    name: str
    offset: int
    dim: int | None
    dim_increment: int
    children: list["SvdRegister | SvdCluster"]


def expand_type(
    type_name: str,
    types: dict[str, list[Member]],
    size_hints: dict[str, int],
    ctx: str,
    used_size_hint_skips: set[str],
    _stack: tuple[str, ...] = (),
) -> tuple[list["SvdRegister | SvdCluster"], int]:
    """*used_size_hint_skips* is mutated in place with every SIZE_HINT_SKIPS
    entry that actually suppressed a real vendor-comment/computed-size
    mismatch -- mirrors FIELD_CROSS_CHECK_SKIPS's "used" tracking (see
    cross_check_file) so generate() can flag a skip whose stale comment
    Renesas has since corrected as an unused, dead entry instead of it
    silently living forever (issue #1029 step 2 second review)."""
    if type_name in _stack:
        fail(f"{ctx}: cyclic cluster reference through {type_name!r}")
    members = types.get(type_name)
    if members is None:
        fail(f"{ctx}: reference to undefined type {type_name!r}")
    def _expand_leaf(leaf: RegisterMember | ClusterMember, offset: int) -> tuple[SvdRegister | SvdCluster, int]:
        if isinstance(leaf, RegisterMember):
            elem_size = SIZE_OF[leaf.elem_type]
            node = SvdRegister(
                name=leaf.name,
                offset=offset,
                elem_bits=elem_size * 8,
                access=ACCESS_OF[leaf.access],
                fields=leaf.fields,
                dim=leaf.count,
            )
            return node, elem_size * (leaf.count or 1)
        children, sub_size = expand_type(
            leaf.type_name, types, size_hints, ctx, used_size_hint_skips, _stack + (type_name,)
        )
        node = SvdCluster(
            name=leaf.name, offset=offset, dim=leaf.count, dim_increment=sub_size, children=children
        )
        return node, sub_size * (leaf.count or 1)

    out: list[SvdRegister | SvdCluster] = []
    offset = 0
    for member in members:
        if isinstance(member, Padding):
            offset += member.nbytes
        elif isinstance(member, Overlay):
            max_size = 0
            for leaf in member.regs:
                node, size = _expand_leaf(leaf, offset)
                out.append(node)
                max_size = max(max_size, size)
            offset += max_size
        elif isinstance(member, (RegisterMember, ClusterMember)):
            node, size = _expand_leaf(member, offset)
            out.append(node)
            offset += size
        else:  # pragma: no cover - exhaustive by construction
            fail(f"{ctx}: unhandled member shape {member!r}")
    hint = size_hints.get(type_name)
    if hint is not None and hint != offset:
        if type_name in SIZE_HINT_SKIPS:
            used_size_hint_skips.add(type_name)
        else:
            fail(
                f"{ctx}: computed size of {type_name} is {offset} bytes, "
                f"vendor comment says {hint} bytes"
            )
    return out, offset


def assign_alternate_registers(children: list["SvdRegister | SvdCluster"], ctx: str) -> None:
    """SVD forbids two `<register>` elements at the same `<addressOffset>`
    within the same parent unless one declares `<alternateRegister>`
    pointing at the other -- svdconv (and any strict loader) rejects the
    collision outright, a tolerant one silently shows only one. This is a
    REAL, if uncommon, vendor pattern, not a parser bug: several `union {
    <reg>; struct {...} <reg>_b; }` arms genuinely overlaid at one address
    (`crc_iodefine.h` CRCDIR/CRCDIR_BY, many `rtc_iodefine.h` BCD-alias
    pairs, `gpio_iodefine.h` IEN2B_L/IEN2B_H) -- exactly what the `Overlay`
    member shape already models.

    Mutates `SvdRegister.alternate_register` in place, recursively: within
    each parent's own child list, the first register at an offset stays
    plain and every later register at the SAME offset points
    `<alternateRegister>` at the first one's name. A collision that is NOT
    purely between two-or-more plain (non-array) registers -- a cluster
    sharing a register's offset, or a register ARRAY involved at all -- is
    an unhandled shape (never seen in the real corpus, see #1029 step 2);
    hard-fail rather than guess at alternateGroup/dim semantics. This
    includes a cluster sharing its offset with a sibling (register OR
    cluster): every node at a colliding offset is considered here, not
    just the `SvdRegister` ones, so a cluster/register collision hard-fails
    exactly like an array does, rather than the cluster silently vanishing
    from consideration.
    """
    by_offset: dict[int, list["SvdRegister | SvdCluster"]] = {}
    for node in children:
        if isinstance(node, SvdCluster):
            assign_alternate_registers(node.children, ctx)
        by_offset.setdefault(node.offset, []).append(node)
    for offset, nodes in by_offset.items():
        if len(nodes) < 2:
            continue
        if any(isinstance(n, SvdCluster) for n in nodes):
            names = ", ".join(n.name for n in nodes)
            fail(
                f"{ctx}: cluster collides with another member at offset 0x{offset:X} "
                f"({names}) -- alternateRegister aliasing across a cluster is unhandled"
            )
        first = nodes[0]
        for other in nodes[1:]:
            if first.dim or other.dim:
                fail(
                    f"{ctx}: register array {other.name!r} collides with {first.name!r} "
                    f"at offset 0x{offset:X} -- alternateRegister aliasing of an array is unhandled"
                )
            other.alternate_register = first.name


# --- SVD (CMSIS-SVD 1.3) XML emission ---------------------------------------


def _dim_index(n: int) -> str:
    return f"0-{n - 1}" if n > 1 else "0"


def _emit_fields(fields: list[tuple[str, int, int, str | None]], indent: str) -> list[str]:
    if not fields:
        return []
    out = [f"{indent}<fields>"]
    for name, lsb, width, note in fields:
        out.append(f"{indent}  <field>")
        out.append(f"{indent}    <name>{xml_escape(name)}</name>")
        if note is not None:
            out.append(f"{indent}    <description>{xml_escape(note)}</description>")
        out.append(f"{indent}    <bitOffset>{lsb}</bitOffset>")
        out.append(f"{indent}    <bitWidth>{width}</bitWidth>")
        out.append(f"{indent}  </field>")
    out.append(f"{indent}</fields>")
    return out


def _emit_node(node: "SvdRegister | SvdCluster", indent: str) -> list[str]:
    out: list[str] = []
    if isinstance(node, SvdRegister):
        name = f"{node.name}[%s]" if node.dim else node.name
        out.append(f"{indent}<register>")
        if node.dim:
            out.append(f"{indent}  <dim>{node.dim}</dim>")
            out.append(f"{indent}  <dimIncrement>0x{(node.elem_bits // 8):X}</dimIncrement>")
            out.append(f"{indent}  <dimIndex>{_dim_index(node.dim)}</dimIndex>")
        out.append(f"{indent}  <name>{xml_escape(name)}</name>")
        if node.alternate_register is not None:
            out.append(f"{indent}  <alternateRegister>{xml_escape(node.alternate_register)}</alternateRegister>")
        out.append(f"{indent}  <addressOffset>0x{node.offset:X}</addressOffset>")
        out.append(f"{indent}  <size>{node.elem_bits}</size>")
        out.append(f"{indent}  <access>{node.access}</access>")
        out.extend(_emit_fields(node.fields, indent + "  "))
        out.append(f"{indent}</register>")
    else:
        name = f"{node.name}[%s]" if node.dim else node.name
        out.append(f"{indent}<cluster>")
        if node.dim:
            out.append(f"{indent}  <dim>{node.dim}</dim>")
            out.append(f"{indent}  <dimIncrement>0x{node.dim_increment:X}</dimIncrement>")
            out.append(f"{indent}  <dimIndex>{_dim_index(node.dim)}</dimIndex>")
        out.append(f"{indent}  <name>{xml_escape(name)}</name>")
        out.append(f"{indent}  <addressOffset>0x{node.offset:X}</addressOffset>")
        for child in node.children:
            out.extend(_emit_node(child, indent + "  "))
        out.append(f"{indent}</cluster>")
    return out


def _cpu_revision(cm33_rev: int) -> str:
    # CMSIS's own __CMx_REV convention: high nibble byte = r-number, low
    # nibble byte = p-number (e.g. Cortex-M3 __CM3_REV 0x0201 => r2p1).
    return f"r{(cm33_rev >> 4) & 0xF}p{cm33_rev & 0xF}"


def build_svd(
    peripherals: list[tuple[str, int, int, list["SvdRegister | SvdCluster"]]],
    cpu: dict[str, int],
) -> str:
    lines = [
        '<?xml version="1.0" encoding="utf-8"?>',
        '<device schemaVersion="1.3" xmlns:xs="http://www.w3.org/2001/XMLSchema-instance" '
        'xs:noNamespaceSchemaLocation="CMSIS-SVD.xsd">',
        "  <vendor>Renesas Electronics Corporation</vendor>",
        "  <vendorID>Renesas</vendorID>",
        "  <name>R9A09G056N</name>",
        "  <series>RZ/V2N</series>",
        "  <version>1.0</version>",
        "  <description>Machine-generated from the vendored hal_renesas FSP headers "
        "by scripts/gen_rzv2n_cm33_svd.py (alp-sdk issue #1029) -- not a Renesas-shipped "
        "artefact, not committed to the repo.</description>",
        "  <licenseText>SPDX-License-Identifier: BSD-3-Clause\n"
        "Copyright (c) 2020-2024 Renesas Electronics Corporation and/or its affiliates\n"
        "(register map mechanically re-expressed from the vendored FSP headers; the "
        "prose above is Alp Lab's, the register facts are Renesas's)</licenseText>",
        "  <cpu>",
        "    <name>CM33</name>",
        f"    <revision>{_cpu_revision(cpu['__CM33_REV'])}</revision>",
        "    <endian>little</endian>",
        f"    <mpuPresent>{'true' if cpu['__MPU_PRESENT'] else 'false'}</mpuPresent>",
        f"    <fpuPresent>{'true' if cpu['__FPU_PRESENT'] else 'false'}</fpuPresent>",
        f"    <dspPresent>{'true' if cpu['__DSP_PRESENT'] else 'false'}</dspPresent>",
        f"    <nvicPrioBits>{cpu['__NVIC_PRIO_BITS']}</nvicPrioBits>",
        "    <vendorSystickConfig>false</vendorSystickConfig>",
        "  </cpu>",
        "  <addressUnitBits>8</addressUnitBits>",
        "  <width>32</width>",
        "  <size>32</size>",
        "  <access>read-write</access>",
        "  <resetValue>0x00000000</resetValue>",
        "  <resetMask>0xFFFFFFFF</resetMask>",
        "  <peripherals>",
    ]
    for name, base_addr, size, children in peripherals:
        lines.append("    <peripheral>")
        lines.append(f"      <name>{xml_escape(name)}</name>")
        lines.append(f"      <baseAddress>0x{base_addr:08X}</baseAddress>")
        # CMSIS-SVD requires >=1 <addressBlock> per non-derivedFrom
        # peripheral; svdconv and strict loaders reject a file without one.
        # Always the SAME extent expand_type() computed by walking the
        # struct -- NEVER the vendor's own `/*!< Size = N */` comment, which
        # can be stale relative to the real member layout (R_CANFD_Type:
        # comment says 0x14000, the walked extent is 0x1C000, see
        # SIZE_HINT_SKIPS). A zero-byte peripheral (a type with no members
        # at all) has nothing to bound; SVD requires a positive size, so
        # this is an unhandled shape, not a guess.
        if size <= 0:
            fail(f"{name}: computed peripheral size is {size} bytes, cannot emit an <addressBlock>")
        lines.append("      <addressBlock>")
        lines.append("        <offset>0x0</offset>")
        lines.append(f"        <size>0x{size:X}</size>")
        lines.append("        <usage>registers</usage>")
        lines.append("      </addressBlock>")
        lines.append("      <registers>")
        for child in children:
            lines.extend(_emit_node(child, "        "))
        lines.append("      </registers>")
        lines.append("    </peripheral>")
    lines.append("  </peripherals>")
    lines.append("</device>")
    return "\n".join(lines) + "\n"


# --- Module-root discovery ---------------------------------------------------


def find_fsp_include_dir(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit)
    sys.path.insert(0, str(REPO / "scripts"))
    from alp_cli._workspace import find_west_topdir  # local import: sys.path just primed

    topdir = find_west_topdir(REPO)
    if topdir is None:
        fail(
            "no west workspace found (walked up from the alp-sdk checkout looking for "
            "a '.west/' directory) -- pass --fsp-include-dir explicitly, or run from "
            "inside a bootstrapped west workspace with the hal_renesas module checked out"
        )
    return (
        topdir
        / "modules"
        / "hal"
        / "renesas"
        / "drivers"
        / "rz"
        / "fsp"
        / "src"
        / "rzv"
        / "bsp"
        / "cmsis"
        / "Device"
        / "RENESAS"
        / "Include"
        / "R9A09G056N"
    )


# --- Orchestration ------------------------------------------------------------


def generate(fsp_include_dir: Path) -> tuple[str, dict[str, int]]:
    """Returns (svd_xml, stats) where stats reports peripheral/skip counts."""
    if not fsp_include_dir.is_dir():
        fail(f"FSP module directory not found: {fsp_include_dir}")
    iodefines_dir = fsp_include_dir / "iodefines"
    iobitmasks_dir = fsp_include_dir / "iobitmasks"
    cpu_header = fsp_include_dir.parent / f"{fsp_include_dir.name}.h"
    for p, what in (
        (iodefines_dir, "iodefines/"),
        (iobitmasks_dir, "iobitmasks/"),
        (cpu_header, cpu_header.name),
    ):
        if not p.exists():
            fail(f"expected {what} at {p}, not found")

    cpu = parse_cpu_header(cpu_header)

    all_types: dict[str, list[Member]] = {}
    all_size_hints: dict[str, int] = {}
    all_instances: list[tuple[str, str, int, str]] = []  # + source file stem
    problems: list[str] = []
    used_field_skips: set[tuple[str, str, str]] = set()
    used_orphan_skips: set[tuple[str, str]] = set()
    used_position_overrides: set[tuple[str, str, str]] = set()

    iodefine_files = sorted(iodefines_dir.glob("*_iodefine.h"))
    if not iodefine_files:
        fail(f"no *_iodefine.h files found under {iodefines_dir}")

    for path in iodefine_files:
        stem = path.stem[: -len("_iodefine")]
        parsed = parse_iodefine_file(path)
        for tname, members in parsed.types.items():
            if tname in all_types:
                fail(f"type {tname!r} defined in more than one iodefine file")
            all_types[tname] = members
        all_size_hints.update(parsed.size_hints)
        for inst, typ, addr in parsed.instances:
            all_instances.append((inst, typ, addr, stem))

        bitmask_path = iobitmasks_dir / f"{stem}_iobitmask.h"
        if not bitmask_path.is_file():
            fail(f"{path.name}: no matching iobitmask file at {bitmask_path}")
        bitmasks = parse_iobitmask_macros(bitmask_path)
        file_problems, file_used_field, file_used_orphan, file_used_override = cross_check_file(
            stem, parsed.field_records, bitmasks
        )
        problems.extend(file_problems)
        used_field_skips |= file_used_field
        used_orphan_skips |= file_used_orphan
        used_position_overrides |= file_used_override

    # "Unused skip-list entry" is only a meaningful defect against the REAL,
    # full corpus (every entry was found by running against it, see the
    # comments on the two skip constants) -- a small test fixture built from
    # a deliberately trimmed excerpt of ONE file has no way to exercise a
    # gpio/ELC_DPTC entry even though that file's stem is "processed", and
    # that's not a defect in the skip list. Gate strict
    # unused-entry enforcement on actually having the real ~26-file corpus
    # (REAL_IODEFINE_FILE_COUNT, verified by listing iodefines/*.h against
    # the real hal_renesas checkout), not merely "some files".
    if len(iodefine_files) >= REAL_IODEFINE_FILE_COUNT:
        unused_field_skips = set(FIELD_CROSS_CHECK_SKIPS) - used_field_skips
        if unused_field_skips:
            fail(f"FIELD_CROSS_CHECK_SKIPS entries never matched a real field: {unused_field_skips}")
        unused_orphan_skips = set(IOBITMASK_ORPHAN_SKIPS) - used_orphan_skips
        if unused_orphan_skips:
            fail(f"IOBITMASK_ORPHAN_SKIPS entries never matched a real macro: {unused_orphan_skips}")
        unused_overrides = set(FIELD_POSITION_OVERRIDES) - used_position_overrides
        if unused_overrides:
            fail(f"FIELD_POSITION_OVERRIDES entries never matched a real field: {unused_overrides}")

    if problems:
        fail("cross-check disagreements between iodefines/ and iobitmasks/:\n  " + "\n  ".join(problems))

    peripherals: list[tuple[str, int, int, list["SvdRegister | SvdCluster"]]] = []
    skipped_peripherals: list[str] = []
    used_size_hint_skips: set[str] = set()
    for inst, typ, addr, stem in all_instances:
        if inst in PERIPHERAL_SKIPS:
            skipped_peripherals.append(inst)
            continue
        ctx = f"{stem} ({inst})"
        children, size = expand_type(typ, all_types, all_size_hints, ctx, used_size_hint_skips)
        assign_alternate_registers(children, ctx)
        peripherals.append((inst, addr, size, children))

    unused_peripheral_skips = set(PERIPHERAL_SKIPS) - set(skipped_peripherals)
    if unused_peripheral_skips:
        fail(f"PERIPHERAL_SKIPS entries never matched a real instance: {unused_peripheral_skips}")

    if len(iodefine_files) >= REAL_IODEFINE_FILE_COUNT:
        unused_size_hint_skips = SIZE_HINT_SKIPS - used_size_hint_skips
        if unused_size_hint_skips:
            fail(f"SIZE_HINT_SKIPS entries never suppressed a real mismatch: {unused_size_hint_skips}")

    peripherals.sort(key=lambda p: (p[1], p[0]))
    svd = build_svd(peripherals, cpu)
    stats = {
        "peripherals": len(peripherals),
        "files": len(iodefine_files),
        # What THIS run actually used, not the static table sizes -- a
        # skip-list entry that never fired isn't reflected in a "ran clean"
        # count (issue #1029 step 2 second review). Field skips and orphan
        # macro skips are reported as two distinct numbers, never lumped:
        # an orphan macro (e.g. CFDTXQOWSTS) is a whole absent register,
        # not a field.
        "field_skips": len(used_field_skips),
        "orphan_skips": len(used_orphan_skips),
        "peripheral_skips": len(skipped_peripherals),
    }
    return svd, stats


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    parser.add_argument("--fsp-include-dir", help="path to .../Include/R9A09G056N (overrides discovery)")
    parser.add_argument("--output", help="write the generated .svd here (required unless --check)")
    parser.add_argument("--check", action="store_true", help="parse and validate, emit nothing")
    args = parser.parse_args(argv)

    if not args.check and not args.output:
        parser.error("--output is required unless --check is given")

    fsp_include_dir = find_fsp_include_dir(args.fsp_include_dir)
    try:
        svd, stats = generate(fsp_include_dir)
    except SvdGenError as exc:
        print(f"gen_rzv2n_cm33_svd: {exc}", file=sys.stderr)
        return 1

    print(
        f"gen_rzv2n_cm33_svd: {stats['peripherals']} peripherals from "
        f"{stats['files']} iodefine files ({stats['field_skips']} field skip(s), "
        f"{stats['orphan_skips']} orphan macro skip(s), "
        f"{stats['peripheral_skips']} peripheral skip(s))"
    )
    if args.check:
        return 0
    Path(args.output).write_text(svd, encoding="utf-8", newline="")
    print(f"gen_rzv2n_cm33_svd: wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
