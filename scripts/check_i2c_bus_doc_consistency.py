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
  carrying a DIFFERENT bus assignment across two presets that both
  declare it is ambiguous ground truth and is silently excluded from
  checking -- never guessed at (`_chip_truth()`).
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
* No address check: round 3 gated a `0xNN`-shaped literal on a
  co-located single bus and single chip, but neither that nor any
  cheaper line-level signal is POSITIVE evidence the literal is a
  7-bit I2C address rather than a register offset, a bitmask, or a
  length that happens to also render as two hex digits (`0x0E` and
  `0x50` are the same shape) on a line that also happens to name a
  bus and a chip -- round 4 review (#1270) found no reliable
  distinguishing signal cheap enough for a line-scoped regex gate that
  does not also reject real address prose (the real corpus's address
  mentions use "at 0xNN", "@0xNN", "0xNN ACK", and bare markdown-table
  cells interchangeably, none of which a register/mask/length mention
  is reliably absent from). `address_7bit` in `i2c_devices` stays
  real, human-authored metadata; this gate just does not cross-check
  doc prose against it. Dropped rather than shipped imprecise a fourth
  time.
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
* `_BUS_ALIASES` is a small hand-maintained table -- no metadata field
  records a doc-facing bus alias to derive it from (the schema's old
  `i2c_bus.bus_pads:` was dead, populated by no preset and read by no
  script, and was dropped rather than wired up here; see #1270). It was
  verified by grep against the tree at write time to carry no
  cross-family collision (`I2C2`/`LPI2C0`/`LPI2C` are
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

  Concretely, against the four wave-4 defects #1270 names verbatim, ONE
  is caught: "the EEPROM was documented on BRD_I2C when it is on SoC
  I2C2" -- fix commit `35cf42ca` names `docs/bring-up-aen.md` as (with
  `docs/soms/aen.md`/`docs/troubleshooting.md`/tutorial 13) the site
  "that originated the error", and its pre-fix `bring-up-aen.md` read
  "**EEPROM / board_id read over BRD_I2C.**  Confirm the 24C128" --
  `24C128` (a real alias) and `BRD_I2C` on one line, which this gate
  flags. The other three are NOT caught: the carrier-part one is out
  of scope by the bullet above; the `examples/aen/aen-secure-element-sign`
  one splits the bus name and the part name across separate sentences;
  and `docs/cc3501e-bridge.md` spells the EEPROM by its abbreviated
  MPN (`N24S128` for `N24S128C4DYT3G`), which this gate's chip-alias
  match never resolves to `eeprom_24c128`, so the mismatch on that line
  is invisible regardless of the bus wording.

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


def _chip_truth(root: Path) -> tuple[dict[str, str], dict[str, str]]:
    """Return (chip_id -> bus_key, chip_id -> every preset path it came
    from, comma-joined) for every chip whose on-module bus assignment
    is IDENTICAL across every SoM preset that declares it. A chip seen
    on two different buses across presets is ambiguous ground truth and
    is dropped -- never guessed at.

    Every preset that agrees is listed, not just the alphabetically
    first one: a chip declared identically on both `E1M-AEN301.yaml`
    and `E1M-AEN801.yaml` should point a reader debugging AEN801 at
    AEN801, not silently cite AEN301 because it sorts first."""
    modules_dir = root / "metadata" / "e1m_modules"
    seen_bus: dict[str, set[str]] = {}
    source: dict[str, set[str]] = {}
    if not modules_dir.is_dir():
        return {}, {}
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
    bus_truth = {cid: next(iter(b)) for cid, b in seen_bus.items() if len(b) == 1}
    return (
        bus_truth,
        {cid: ", ".join(sorted(source[cid])) for cid in bus_truth},
    )


def _bus_pattern(bus_key: str) -> re.Pattern[str]:
    literal = bus_key.upper()
    # The portable-API instance ID spells some bus keys `ALP_`-prefixed
    # (#1270's own issue text writes "ALP_E1M_I2C0", and it is the tree's
    # dominant spelling for e1m_i2c0 -- measured over `git ls-files`:
    # 171 `ALP_E1M_I2C0` against 86 bare `E1M_I2C0`).
    #
    # Both spellings have to be offered, and the reason is not that ratio:
    # a bare `\bE1M_I2C0\b` can never match inside `ALP_E1M_I2C0` at all,
    # because the `_` immediately before `E1M` is itself a word character,
    # so `\b` finds no boundary there. Matching only the bare form would
    # miss every prefixed site no matter which spelling were commoner.
    names = {literal, f"ALP_{literal}"} | _BUS_ALIASES.get(bus_key, set())
    alt = "|".join(re.escape(n) for n in sorted(names, key=len, reverse=True))
    return re.compile(rf"\b(?:{alt})\b", re.IGNORECASE)


def find_problems(root: Path) -> list[str]:
    """Pure check: returns human-readable problem strings, empty when
    every checked doc/example line agrees with the i2c_devices ground
    truth (or names no ground-truth chip / no single bus)."""
    chip_bus, chip_source = _chip_truth(root)
    if not chip_bus:
        return []

    chip_mpns = _load_chip_mpns(root)
    chip_aliases = {
        cid: _chip_aliases(cid, chip_mpns.get(cid, [])) for cid in chip_bus
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
                    # No single-bus claim on this line -- typically a
                    # deliberate contrast ("I2C2, NOT LPI2C0"). Skipped
                    # rather than risk a false positive on it.
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
