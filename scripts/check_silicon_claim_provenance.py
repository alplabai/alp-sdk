#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Require a hardware-value-backed silicon/bench-verification claim in a
changelog.d/ fragment to be corroborated somewhere outside that fragment
(issue #1993).

WHY THIS EXISTS
---------------
On 2026-09-05, `changelog.d/1981.md` shipped: "Bench-verified on two
E1M-AEN803 modules on 2026-09-05: the Secure Enclave's own boot table
booted BOOTLOAD (TF-A BL32) at cpu_id A32_0, boot address 0x80002000,
flags u VB." None of it happened -- no such bench session ran, no
AEN803 module is on any bench, and `0x80002000` was actually copied
from an Alif application-note config, not measured. The same fabricated
sentence was ALSO the sole comment justifying `0x80002000` as a
production constant in `scripts/aen_atoc.py`'s `SLOT0_WINDOWS`, and it
reached two issue bodies besides. Every existing gate --
`check_changelog_citations.py`, `check_atoc_reservation.py`,
`validate_metadata.py`, `check_doc_drift.py`, the ATOC pytest suite --
was green throughout, because none of them ask whether a hardware claim
is TRUE. Withdrawn in `b1de21838` and `5597f8265` (PR #1991).

This gate cannot tell true from false prose -- no gate can. What it CAN
check mechanically is the one thing the retraction commit itself used to
catch this: "the address appears nowhere else in the tree -- not in
docs/, not in metadata/e1m_modules/, not in docs/test-plan.md". A real
bench measurement of a specific address/register value leaves a trail
somewhere that is not the one narrative sentence asserting it; a
fabricated one does not.

WHAT COUNTS AS A CLAIM
-----------------------
A `changelog.d/*.md` SENTENCE (paragraphs split on blank lines, then on
sentence-ending punctuation) that BOTH:

  1. matches a verification-claim phrase -- "bench-verified",
     "bench-proven", "bench-measured", "measured on", "confirmed on N of
     N", "silicon-verified", or "verified on silicon" / "verified on real
     silicon" (case-insensitive); AND
  2. names a concrete hardware value in that same sentence -- a hex
     literal (`0x1234...`) or a bench-serial token in the
     `YYYYWnn-nnnn` shape this repo's provisioning tool uses (e.g.
     `2026W36-0001`, see `scripts/program_eeprom.py --serial`).

Sentence-level, not paragraph-level, is deliberate: an early draft grouped
by paragraph and mis-fired on the #1981 fragment's OWN prior, unrelated
sentence ("No in-tree flash path could stage an A32 Linux image.") --
its "No" isn't a disclaimer of the bench claim two sentences later, but a
paragraph-wide negation search read it as one and would have passed the
real defect.

A sentence containing an explicit negation ("not", "never", "cannot",
"unverified", "did not", ...) anywhere in it is read as a DISCLAIMER, not
a claim, and is exempt -- this is what keeps the withdrawn #1981 text
itself (which now says "NOT verified on silicon" right next to the same
`0x80002000`, in one sentence) passing after the fix landed.

WHAT COUNTS AS PROVENANCE
--------------------------
The exact hex/serial token must appear somewhere under `docs/` (excluding
`docs/superpowers/**`, dated planning/spec dumps, not reviewed hardware
record) or `metadata/` in the SAME tree, OUTSIDE `changelog.d/`. This
mirrors the retraction commit's own standard verbatim, and deliberately
does NOT accept another `scripts/*.py`, `chips/**`, `src/**` or
`include/**` file as corroboration -- issue #1993's own incident put the
identical fabricated sentence in `scripts/aen_atoc.py` at the same time
as the changelog fragment, so treating a second copy of the same
unsourced comment as "corroboration" would have passed the real defect,
not caught it. `docs/test-plan.md` (the verification system of record,
see the v0.15.0 clean-cut decision) is naturally included as it lives
under `docs/`.

WHAT THIS CATCHES
------------------
A changelog.d/ fragment asserting bench/silicon verification of a
specific address or serial that has no independent documentation or
metadata trail anywhere in the tree -- the exact shape of the #1981
incident.

WHAT THIS DOES NOT CATCH -- read this before trusting it
----------------------------------------------------------
* A narrative claim with NO hex/serial anchor ("bench-verified on
  e1m-aen-evk-01: all four presets report radio_ok=1" -- changelog.d/
  1679.md) is not checked at all. Requiring every such sentence to cite
  an external record was tried against this tree first and flagged
  roughly 20 of 24 existing, correctly-sourced fragments (no matching
  `docs/test-plan.md` row exists for most changelog issue numbers) --
  exactly the noisy-gate failure #1963 was rejected for. Narrower and
  under-flagging beats that.
* `scripts/*.py` / `chips/**` / `src/**` / `include/**` comments are not
  scanned as CLAIM sources, only consulted (and rejected) as would-be
  corroboration. A fabricated bench claim planted only as a source-code
  comment, never in a changelog fragment, is invisible to this gate.
* `CHANGELOG.md` and `docs/superpowers/**` are excluded from BOTH claim
  scanning and corroboration, deliberately. `CHANGELOG.md` is a
  byte-for-byte fold of `changelog.d/` fragments (`assemble_changelog.py`)
  -- scanning it re-flags the same fragment twice, and counting it as
  corroboration for its own source fragment is circular. Files under
  `docs/superpowers/` are raw planning/session dumps, not reviewed
  hardware records; the same unsourced sentence copy-pasted there would
  otherwise "corroborate" itself.
* Even a genuinely corroborated hex value is not proof of truth --
  `docs/` or `metadata/` merely have to also mention the number. This
  gate narrows "unsourced" to "not sourced anywhere in this repo", not
  to "true". It does not replace the reviewer `inventedHardwareFacts`
  verdict issue #1993 also calls for.

To satisfy this gate for a real, new hardware-address bench result: add
the address to `docs/test-plan.md` (or another `docs/` / `metadata/`
file) in the same change that adds the changelog fragment.

Exit codes:
    0  every hex/serial-anchored verification claim is corroborated
    1  at least one is not
    2  usage / environment error

Run locally:

    python3 scripts/check_silicon_claim_provenance.py
    python3 scripts/check_silicon_claim_provenance.py --root <dir>
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

CLAIM_RE = re.compile(
    r"bench-verified|bench-proven|bench-measured|measured on|"
    r"confirmed on \d+ of \d+|silicon-verified|"
    r"verified on (?:real )?silicon",
    re.IGNORECASE,
)

NEGATION_RE = re.compile(
    r"\bnot\b|\bno\b|\bnever\b|\bcannot\b|\bcan't\b|\bcan not\b|\bisn't\b|"
    r"\bwasn't\b|\bdoesn't\b|\bdon't\b|\bdid not\b|\bdoes not\b|"
    r"\bunverified\b|\bunattested\b|\bunattributed\b",
    re.IGNORECASE,
)

HEX_RE = re.compile(r"0x[0-9A-Fa-f]{4,}")
#: The `YYYYWnn-nnnn` bench/production serial shape `scripts/program_eeprom.py
#: --serial` and its tests use (e.g. `2026W36-0001`).
SERIAL_RE = re.compile(r"\b\d{4}W\d{2}-\d{4}\b")

#: Corroboration pool: independently-maintained record, not narrative prose
#: and not the attack surface itself (see module docstring). Relative to
#: the tree root.
PROVENANCE_DIRS = ("docs", "metadata")
#: Raw planning/session dumps under docs/ -- not a reviewed hardware record.
PROVENANCE_EXCLUDE_PREFIXES = ("docs/superpowers/",)

SKIP_DIR_NAMES = {"node_modules", "__pycache__", "build", "dist", "venv", ".venv"}


def _sentences(text: str) -> list[str]:
    """Split into paragraphs on blank lines, then into sentences on
    sentence-ending punctuation. Crude on purpose -- see module docstring
    for why paragraph-level grouping is too coarse."""
    out: list[str] = []
    for para in re.split(r"\n\s*\n", text):
        for sentence in re.split(r"(?<=[.!?])\s+", para):
            if sentence.strip():
                out.append(sentence)
    return out


def _anchors(unit: str) -> list[str]:
    return HEX_RE.findall(unit) + SERIAL_RE.findall(unit)


def _claims(fragment: Path) -> list[tuple[str, list[str]]]:
    """[(sentence, anchor_tokens)] for every unsourced-shaped claim in
    `fragment` -- a sentence with a claim phrase, a hex/serial anchor, and
    no negation cue."""
    text = fragment.read_text(encoding="utf-8", errors="replace")
    hits: list[tuple[str, list[str]]] = []
    for sentence in _sentences(text):
        if not CLAIM_RE.search(sentence):
            continue
        anchors = sorted(set(_anchors(sentence)))
        if not anchors:
            continue
        if NEGATION_RE.search(sentence):
            continue
        hits.append((sentence, anchors))
    return hits


def _provenance_haystack(root: Path) -> str:
    """Concatenated text of every file under PROVENANCE_DIRS (minus the
    excluded prefixes) -- read once, substring-checked many times."""
    chunks: list[str] = []
    for dirname in PROVENANCE_DIRS:
        base = root / dirname
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if not path.is_file():
                continue
            rel = path.relative_to(root).as_posix()
            if rel.startswith(PROVENANCE_EXCLUDE_PREFIXES):
                continue
            if any(part in SKIP_DIR_NAMES for part in path.parts):
                continue
            try:
                chunks.append(path.read_text(encoding="utf-8", errors="ignore"))
            except OSError:
                continue
    return "\n".join(chunks)


def find_problems(root: Path) -> list[str]:
    frag_dir = root / "changelog.d"
    if not frag_dir.is_dir():
        return []
    haystack = _provenance_haystack(root)
    problems: list[str] = []
    for fragment in sorted(frag_dir.glob("*.md")):
        for block, anchors in _claims(fragment):
            uncorroborated = [a for a in anchors if a not in haystack]
            if not uncorroborated:
                continue
            rel = fragment.relative_to(root).as_posix()
            snippet = " ".join(block.split())[:160]
            problems.append(
                f"{rel}: silicon-verification claim cites "
                f"{', '.join(uncorroborated)} with no corroborating "
                f"docs/ or metadata/ reference anywhere in the tree "
                f"(see scripts/check_silicon_claim_provenance.py): {snippet}"
            )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", type=Path, default=REPO,
                     help="repository root to check (default: this repo)")
    args = ap.parse_args()

    problems = find_problems(args.root)
    if problems:
        print("check_silicon_claim_provenance: unsourced silicon claim(s):",
              file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    print("check_silicon_claim_provenance: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
