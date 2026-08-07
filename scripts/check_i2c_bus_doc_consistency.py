#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: doc/example prose that names a known on-module I2C part next to
an I2C bus name must agree with that part's bus in the owning SoM
preset's `on_module.i2c_devices:` block. #1270 added this block to
`metadata/e1m_modules/E1M-AEN801.yaml` and its five AEN siblings
(`E1M-V2N101.yaml` / `E1M-V2M101.yaml` already carried one); this gate
is the cross-check #1270 asked for, reading across every SoM preset --
not AEN's alone -- mirroring what check_pin_conflicts.py does for
silicon pads.

Before #1270, "which bus is this part on" existed only in prose, spread
across docs/bring-up-aen.md, docs/soms/aen.md, docs/troubleshooting.md,
docs/cc3501e-bridge.md, several examples/aen/* READMEs and their
src/main.c comments -- with no source of truth to check any of them
against. Four consecutive wave-4 doc passes each corrected one bus
assignment and broke another.

Scope -- a NARROW, LINE-scoped gate, deliberately:

* Ground truth is CROSS-FAMILY, built from `on_module.i2c_devices`
  blocks across every `metadata/e1m_modules/E1M-*.yaml` preset, not
  AEN's six alone -- a chip is one ground-truth entry as long as every
  preset that declares it agrees, regardless of family. A chip
  carrying a DIFFERENT bus (or address_7bit) assignment across two
  presets that both declare it is ambiguous ground truth and is
  silently excluded from checking -- never guessed at (`_chip_truth()`).
  Concretely: every chip AEN's own six blocks declare (OPTIGA Trust M,
  RV-3028-C7, TMP112, the 24C128 EEPROM) already had an identical
  V2N/V2M declaration before #1270 landed, so deleting all six AEN
  blocks today changes zero reported problems -- only the
  `chip_source` provenance list each problem cites shrinks (10 presets
  to 4). AEN's own block earns its keep the day AEN ever needs a value
  that disagrees with V2N/V2M (at which point cross-family agreement
  breaks and the chip drops out of ground truth entirely, per this
  bullet) or for a direct per-SoM lookup outside this gate -- not
  because today's coverage depends on it.
* A doc LINE is checked for a bus mismatch only when it names exactly
  ONE bus (by the bus key's own literal spelling, its `ALP_`-prefixed
  portable-API form, or one of the human-readable aliases in
  `_BUS_ALIASES`) -- a line naming two buses is typically a deliberate
  contrast ("I2C2, NOT LPI2C0") and is skipped rather than risk a
  false positive on it.
* An address claim is checked ONLY on a line that ALSO passes that same
  single-bus gate, names exactly one `0xNN`-shaped 7-bit address
  literal, AND names exactly one ground-truth chip -- a single `0xNN`
  literal cannot belong to more than one chip, so a line naming two
  known chips (a table row, "the trio" listed together) is ambiguous
  about which chip the address claims and is skipped rather than
  attributed to every chip named on the line (review round 2: a
  correct row "| TMP112 | RV-3028-C7 | 0x48 | BRD_I2C |" false-
  positived on RV-3028-C7, whose address was never claimed on that line
  at all). An 0xNN literal with no co-located bus name is far more
  often a register/opcode/sentinel value (`0x82` I2C_STATE, `0xFF` a
  DNP sentinel, `0x32` ADC_CONFIGURE's opcode) than an I2C address
  claim, and checking it unanchored produced 16 false positives across
  `docs/superpowers/**` and `examples/**/*.c` alone.
* Chip mentions are matched per WHITESPACE-SPLIT TOKEN (each token
  alnum-squashed, then compared for exact set membership against a
  chip's alias set) -- never a substring search across the whole
  squashed line. A substring search let a short generic alias
  (`TRUST`, out of `optiga_trust_m`) false-positive inside an unrelated
  word (`TRUSTed-boot`, `TrustZone-M`) that happens to share no real
  token boundary once punctuation is squashed away. Token-exact
  matching alone is not enough, though: a chip-id underscore-part that
  is itself an ordinary English word (`trust`) is still its own
  standalone token and false-positives on unrelated prose that happens
  to use that word (review round 2: "You can trust the I2C2 scan
  output..."). `_GENERIC_PARTS` excludes such words from the alias set
  by name -- the full chip-id squash (`OPTIGATRUSTM`) and non-generic
  parts (`OPTIGA`) still match.
* `_BUS_ALIASES` is a small hand-maintained table, not derived from
  `bus_pads:` prose -- verified by grep against the tree at write time
  to carry no cross-family collision (`I2C2`/`LPI2C0`/`LPI2C` are
  AEN-only spellings today; `ALP_E1M_X_I2C2` is a different, unrelated
  E1M-X port-identity namespace and is never matched here because
  those lines never also name one of these on-module parts). A new
  family's own physical-bus spelling needs a matching alias added
  here, not a generic regex.
* Board-side (carrier) parts recorded in `metadata/boards/*.yaml`
  `populated:` blocks are OUT OF SCOPE -- this gate only checks
  on-module parts declared in an `i2c_devices:` block. A prose line
  naming a carrier-side-only part (e.g. the AEN EVK's TCAL9538 /
  INA236 / BMP581) is never flagged, even if it disagrees with reality.
* `docs/superpowers/plans/**` and `docs/superpowers/specs/**` are OUT
  OF SCOPE -- archival bench-session notes and pre-cleanup design docs
  that deliberately record a PAST state (same carve-out
  `check_cross_platform.py`'s `DEFAULT_EXCLUDES` already makes). A
  later, correct metadata change must not force an edit to a document
  recording what was true when it was written.
* Being LINE-scoped, a bus/address claim split across two lines (a
  bus named in one sentence, the part named only in the next) is
  invisible to this gate -- it does not track topic across lines or
  paragraphs. A doc/example MPN spelling that is a truncated/abbreviated
  form of `mpn_population` (not the full part number, e.g. `N24S128`
  for the EEPROM's `N24S128C4DYT3G`) is also not derived as an alias.

  Concretely, against the four wave-4 defects #1270 names verbatim,
  NONE is caught: the carrier-part one is out of scope by the bullet
  above; the `examples/aen/aen-secure-element-sign` one splits the bus
  name and the part name across separate sentences; and the
  `docs/soms/aen.md` / `docs/cc3501e-bridge.md` ones each name the
  EEPROM on a line that ALSO names the other, correct-contrast bus in
  the same breath ("SoC I2C2 ... NOT LPI2C0" / "LPI2C0; EEPROM ...
  SoC I2C2") -- the deliberate two-bus skip above defeats them
  regardless of alias coverage, and both also spell the EEPROM by its
  abbreviated MPN (`N24S128`), an independent second reason this gate
  would not catch them even on a single-bus line. What the gate DOES
  catch is the same underlying EEPROM-on-BRD_I2C defect class at a
  related pre-wave-4 site the issue doesn't name individually,
  `docs/bring-up-aen.md`, which happened to spell the part `24C128` (a
  real alias) on a single-bus line instead.

Run locally:

    python3 scripts/check_i2c_bus_doc_consistency.py

CI wires this in `pr-metadata-validate.yml`.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent
MODULES_DIR = REPO / "metadata" / "e1m_modules"
CHIPS_DIR = REPO / "metadata" / "chips"

# Human-readable spellings docs use for a machine `i2c_devices` bus key,
# beyond the key's own literal spelling (`BRD_I2C` / `E1M_I2C0`, always
# matched too). See the module docstring's "Scope" section for the
# collision check this table relies on.
_BUS_ALIASES: dict[str, set[str]] = {
    "brd_i2c": {"LPI2C0", "LPI2C"},
    "e1m_i2c0": {"I2C2"},
}

# 7-bit I2C address literal, as docs/examples spell it: `0x` + exactly two
# hex digits (`0x48`, `0x30`, ... -- matches `address_7bit` in the SoM
# preset). Compared case-insensitively; lower()'d before comparison.
_ADDR_RE = re.compile(r"\b0x[0-9A-Fa-f]{2}\b")

# Chip-id name parts too generic to serve as a standalone doc alias on
# their own -- either a role word (`eeprom`) or an ordinary English word
# (`trust`) that collides with unrelated prose anywhere in the docs tree
# regardless of token-exact matching (review round 2: "You can trust the
# I2C2 scan output..." false-positived on `optiga_trust_m`'s `trust`
# part). The full chip-id squash (`OPTIGATRUSTM`) and the non-generic
# parts (`OPTIGA`) still match; only the standalone dictionary word is
# excluded.
_GENERIC_PARTS = {"eeprom", "chip", "sensor", "clock", "clk", "trust"}

_NON_ALNUM_RE = re.compile(r"[^A-Za-z0-9]+")

DOC_GLOBS = ("docs/**/*.md", "examples/**/*.md", "examples/**/*.c")

# `docs/superpowers/plans/` and `docs/superpowers/specs/` are archival --
# dated bench-session notes and pre-cleanup design docs that deliberately
# record a PAST state, not living documentation (same carve-out as
# `check_cross_platform.py`'s `DEFAULT_EXCLUDES` and
# `lint_doc_yaml_fragments.py`'s default excludes). A later, correct
# metadata change must not force an edit to a document that is recording
# what was true when it was written (review round 2: flipping tmp112's
# bus in every preset reddened 3 sites under `docs/superpowers/plans/**`
# that were accurate history, not live drift).
_ARCHIVAL_PREFIXES = ("docs/superpowers/plans/", "docs/superpowers/specs/")


def _squash(text: str) -> str:
    """Uppercase, alphanumeric-only projection, used so a chip name
    matches regardless of doc punctuation (`RV-3028-C7` and the
    metadata's `rv3028c7` chip-id both squash to `RV3028C7`)."""
    return _NON_ALNUM_RE.sub("", text).upper()


def _squash_tokens(line: str) -> set[str]:
    """Whitespace-split, alnum-squashed tokens for one doc line.

    Unlike squashing the WHOLE line (which deletes spaces too and would
    merge `The trusted-boot flow` into one blob that `TRUST` matches as
    a substring), squashing per whitespace-split token keeps a real word
    boundary at every space while still tolerating in-token punctuation
    (`RV-3028-C7` squashes to the same `RV3028C7` as the metadata's
    `rv3028c7` chip-id -- exactly the property `_squash()` exists for)."""
    return {_squash(tok) for tok in line.split() if tok}


def _chip_aliases(chip_id: str, mpn_population: list[str]) -> set[str]:
    """Doc-name candidates for one chip: the full squashed chip-id, its
    non-generic squashed underscore-parts (e.g. `24c128` out of
    `eeprom_24c128`), and every squashed `mpn_population` entry."""
    aliases = {_squash(chip_id)}
    for part in chip_id.split("_"):
        squashed = _squash(part)
        if len(squashed) >= 4 and part not in _GENERIC_PARTS:
            aliases.add(squashed)
    for mpn in mpn_population:
        squashed = _squash(mpn)
        if squashed:
            aliases.add(squashed)
    return {a for a in aliases if a}


def _load_chip_mpns(root: Path) -> dict[str, list[str]]:
    mpns: dict[str, list[str]] = {}
    chips_dir = root / "metadata" / "chips"
    if not chips_dir.is_dir():
        return mpns
    for f in sorted(chips_dir.glob("*.yaml")):
        with f.open(encoding="utf-8") as fh:
            data = yaml.safe_load(fh) or {}
        cid = data.get("chip_id")
        if cid:
            mpns[cid] = [str(m) for m in data.get("mpn_population", [])]
    return mpns


def _chip_truth(
    root: Path,
) -> tuple[dict[str, str], dict[str, str], dict[str, str]]:
    """Return (chip_id -> bus_key, chip_id -> address_7bit, chip_id ->
    every preset path it came from, comma-joined) for every chip whose
    on-module bus assignment / address is IDENTICAL across every SoM
    preset that declares it. A chip seen on two different buses (or two
    different addresses) across presets is ambiguous ground truth and
    is dropped from the corresponding dict -- never guessed at.
    `address_7bit` is optional in the schema, so a chip can appear in
    the bus dict without appearing in the address dict.

    Every preset that agrees is listed, not just the alphabetically
    first one: a chip declared identically on both `E1M-AEN301.yaml`
    and `E1M-AEN801.yaml` should point a reader debugging AEN801 at
    AEN801, not silently cite AEN301 because it sorts first."""
    modules_dir = root / "metadata" / "e1m_modules"
    seen_bus: dict[str, set[str]] = {}
    seen_addr: dict[str, set[str]] = {}
    source: dict[str, set[str]] = {}
    if not modules_dir.is_dir():
        return {}, {}, {}
    for preset in sorted(modules_dir.glob("E1M-*.yaml")):
        with preset.open(encoding="utf-8") as f:
            data = yaml.safe_load(f) or {}
        buses = ((data.get("on_module") or {}).get("i2c_devices")) or {}
        for bus_key, bus in buses.items():
            for dev in (bus or {}).get("devices", []) or []:
                cid = dev.get("chip")
                if not cid:
                    continue
                seen_bus.setdefault(cid, set()).add(bus_key)
                source.setdefault(cid, set()).add(
                    str(preset.relative_to(root).as_posix()))
                addr = dev.get("address_7bit")
                if addr:
                    seen_addr.setdefault(cid, set()).add(str(addr).strip().lower())
    bus_truth = {cid: next(iter(b)) for cid, b in seen_bus.items() if len(b) == 1}
    addr_truth = {cid: next(iter(a)) for cid, a in seen_addr.items() if len(a) == 1}
    # Union of both truth dicts' keys: a chip can have an unambiguous
    # address but an ambiguous bus across presets (or vice versa), and
    # the address-check path below looks `chip_source[cid]` up keyed
    # only on `addr_truth` membership -- restricting this to `bus_truth`
    # would KeyError the first time that split ever occurs.
    cids = set(bus_truth) | set(addr_truth)
    return (
        bus_truth,
        addr_truth,
        {cid: ", ".join(sorted(source[cid])) for cid in cids},
    )


def _bus_pattern(bus_key: str) -> re.Pattern[str]:
    literal = bus_key.upper()
    # The portable-API instance ID spells some bus keys `ALP_`-prefixed
    # (#1270's own issue text writes "ALP_E1M_I2C0", and it is the
    # tree's dominant spelling for e1m_i2c0 -- 66 sites vs. 0 for the
    # bare `E1M_I2C0`). A bare `\bE1M_I2C0\b` can never match inside
    # `ALP_E1M_I2C0`: the `_` immediately before `E1M` is itself a word
    # character, so `\b` finds no boundary there. Always offer the
    # `ALP_`-prefixed form as an alternate spelling too.
    names = {literal, f"ALP_{literal}"} | _BUS_ALIASES.get(bus_key, set())
    alt = "|".join(re.escape(n) for n in sorted(names, key=len, reverse=True))
    return re.compile(rf"\b(?:{alt})\b", re.IGNORECASE)


def find_problems(root: Path) -> list[str]:
    """Pure check: returns human-readable problem strings, empty when
    every checked doc/example line agrees with the i2c_devices ground
    truth (or names no ground-truth chip / no single bus or address)."""
    chip_bus, chip_addr, chip_source = _chip_truth(root)
    if not chip_bus:
        return []

    chip_mpns = _load_chip_mpns(root)
    chip_ids = set(chip_bus) | set(chip_addr)
    chip_aliases = {
        cid: _chip_aliases(cid, chip_mpns.get(cid, [])) for cid in chip_ids
    }
    bus_patterns = {bus: _bus_pattern(bus) for bus in sorted(set(chip_bus.values()))}

    problems: list[str] = []
    for pattern in DOC_GLOBS:
        for path in sorted(root.glob(pattern)):
            rel = path.relative_to(root).as_posix()
            if rel.startswith(_ARCHIVAL_PREFIXES):
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            for lineno, line in enumerate(text.splitlines(), start=1):
                # Token-exact match, never a substring search across the
                # whole squashed line -- see the module docstring's
                # "Scope" section on why (the TRUST/TRUSTED-boot false
                # positive).
                line_tokens = _squash_tokens(line)
                mentioned_cids = {
                    cid for cid, aliases in chip_aliases.items()
                    if aliases & line_tokens
                }
                if not mentioned_cids:
                    continue

                mentioned_buses = {
                    bus for bus, pat in bus_patterns.items() if pat.search(line)
                }
                if len(mentioned_buses) != 1:
                    # No single-bus claim on this line -- an 0xNN literal
                    # here is far more often a register/opcode/sentinel
                    # value (0x82 I2C_STATE, 0xFF DNP sentinel, 0x32
                    # ADC_CONFIGURE opcode) than a 7-bit I2C address
                    # claim. Requiring a co-located bus name is what
                    # tells the two apart.
                    continue
                (doc_bus,) = mentioned_buses
                for cid in mentioned_cids:
                    true_bus = chip_bus.get(cid)
                    if true_bus is not None and true_bus != doc_bus:
                        problems.append(
                            f"{rel}:{lineno}: names '{cid}' next to bus "
                            f"'{doc_bus}', but {chip_source[cid]} "
                            f"on_module.i2c_devices puts '{cid}' on "
                            f"'{true_bus}' -- fix the doc or the "
                            f"metadata: {line.strip()!r}"
                        )

                addr_matches = _ADDR_RE.findall(line)
                if len(addr_matches) != 1 or len(mentioned_cids) != 1:
                    # One 0xNN literal cannot belong to more than one
                    # chip. A line naming two known chips alongside one
                    # address (a table row, "the trio" listed together)
                    # is ambiguous about which chip the address claims --
                    # skipped rather than attributing it to every chip
                    # named on the line (review round 2: a correct row
                    # "| TMP112 | RV-3028-C7 | 0x48 | BRD_I2C |" false-
                    # positived on rv3028c7, which is on brd_i2c but at a
                    # different address). Mirrors the single-bus rule
                    # above.
                    continue
                (cid,) = mentioned_cids
                doc_addr = addr_matches[0].lower()
                true_addr = chip_addr.get(cid)
                if true_addr is not None and true_addr != doc_addr:
                    problems.append(
                        f"{rel}:{lineno}: names '{cid}' at address "
                        f"'{doc_addr}' next to bus '{doc_bus}', but "
                        f"{chip_source[cid]} on_module.i2c_devices puts "
                        f"'{cid}' at '{true_addr}' -- fix the doc or "
                        f"the metadata: {line.strip()!r}"
                    )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    problems = find_problems(args.root)

    if problems:
        print(
            "i2c-bus-doc-consistency: doc/example prose disagrees with "
            "on_module.i2c_devices metadata:",
            file=sys.stderr,
        )
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        print(
            f"\ni2c-bus-doc-consistency: {len(problems)} problem(s) -- failing.",
            file=sys.stderr,
        )
        return 1

    print("i2c-bus-doc-consistency: OK (no doc/example bus-name mismatch found).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
