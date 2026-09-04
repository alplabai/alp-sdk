#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Public/private classifier gate for the public alp-sdk repository.

The public repo may describe customer-facing SDK facts, supported build
interfaces, and sanitized metadata.  It must not carry private hardware-design
references, internal audit/report names, maintainer workstation paths,
schematic-level SoM pad/test-point detail, dangling links into the
maintainer's local AI-memory store, internal lab-bench identifiers (labgrid
place names, probe serials, SSH endpoints), or PCB-routing implementation
detail.  This checker keeps that boundary mechanical for the text surfaces
most likely to drift.

Default scan roots:

  README.md, CHANGELOG.md, VERSIONS.md, docs/, include/, metadata/,
  examples/, firmware/, scripts/, src/, chips/, blocks/, tests/, tools/,
  zephyr/, meta-alp-sdk/, .github/workflows/

Generated ABI snapshots, vendored code, and build outputs are always
skipped.  ``docs/superpowers`` (raw internal planning notes) is scanned by
every rule, no exemptions -- it is precisely the tree lab IP has leaked
from before (issue #524).  Issue #1515 measured a per-rule opt-out for
``PRIVATE_AUDIT_REFERENCE`` (its "audit"/"report" vocabulary was thought to
false-positive on planning prose describing the private-notes system
itself) against the live tree -- 98 files / 50,112 lines of
docs/superpowers, forcing that rule on -- and got zero hits: the
false-positive risk the exemption was meant to absorb never materialized,
so there is nothing for a per-rule flag to buy.  The patterns are
intentionally narrow; normal uses of "internal" for an on-chip reference,
software implementation detail, or customer/private extension repo are not
findings.

Local invocation:

  python3 scripts/check_public_private.py
  python3 scripts/check_public_private.py --path docs/aen-provisioning.md
  python3 scripts/check_public_private.py --json
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


REPO = Path(__file__).resolve().parent.parent

DEFAULT_ROOTS: tuple[str, ...] = (
    "README.md",
    "CHANGELOG.md",
    "VERSIONS.md",
    "docs",
    "include",
    "metadata",
    "examples",
    "firmware",
    "scripts",
    "src",
    "chips",
    "blocks",
    "tests",
    "tools",
    "zephyr",
    "meta-alp-sdk",
    ".github/workflows",
)

DEFAULT_EXCLUDES: tuple[str, ...] = (
    ".git",
    "build",
    "node_modules",
    "vendors",
    "docs/abi",
    "__pycache__",
)

TEXT_SUFFIXES: frozenset[str] = frozenset({
    ".bb",
    ".bbappend",
    ".c",
    ".cmake",
    ".conf",
    ".dtsi",
    ".h",
    ".inc",
    ".json",
    ".kconfig",
    ".md",
    ".overlay",
    ".ps1",
    ".py",
    ".sh",
    ".txt",
    ".yaml",
    ".yml",
})

TEXT_FILENAMES: frozenset[str] = frozenset({
    "CMakeLists.txt",
    "Kconfig",
    "Kconfig.alp-libraries",
})


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    category: str
    matched_text: str
    suggestion: str


@dataclass(frozen=True)
class Rule:
    category: str
    pattern: re.Pattern[str]
    suggestion: str
    # Whether a match is exempt when the enclosing bullet/paragraph itself
    # states that the cited artifact left the public tree (see
    # _describes_removal below).  Default False.  PRIVATE_AUDIT_REFERENCE is
    # the only rule that opts in: a paragraph that says "X removed from the
    # public SDK ... moved to <private-repo>" is the audit trail of a scrub,
    # not a leak -- it reproduces no audit content, only records that a
    # relocation happened.  Scoped to one rule, not global, so this can never
    # quietly relax LOCAL_MAINTAINER_PATH, PRIVATE_DESIGN_REFERENCE, or the
    # SoM/PCB detail rules, whose matches are never legitimate to explain
    # away this way.
    exempt_if_describes_removal: bool = False


@dataclass(frozen=True)
class PendingExemption:
    """One (path, category, line-range) carve-out for a maintainer decision
    still in flight.  Narrow by construction: it exempts exactly one known
    location from exactly one rule, so the same phrase anywhere else --
    including a second paragraph in the same file -- still fails the gate."""
    path: str
    category: str
    line_start: int
    line_end: int
    reason: str


# Known-pending items under active maintainer review.  Do not add an entry
# here to make a NEW finding go away -- fix the finding, or take it to the
# maintainer and cite the issue/PR that is actually deciding it.
#
# Empty right now: the one entry this ever held (CHANGELOG.md's Renesas
# Ethernet/eMMC/uSD/xSPI pin-assignment paragraph, PCB_ROUTING_DETAIL,
# issue #524) was resolved by maintainer ruling -- redact -- and the
# paragraph was rewritten to drop the routing detail outright, so the
# exemption is gone rather than converted into a permanent allowlist entry.
KNOWN_PENDING: tuple[PendingExemption, ...] = ()


def _is_known_pending(rel: str, category: str, line_no: int) -> bool:
    return any(
        rel == p.path and category == p.category and p.line_start <= line_no <= p.line_end
        for p in KNOWN_PENDING
    )


@dataclass(frozen=True)
class ReviewedAcceptedEntry:
    """One (path, line, category) finding a human read and accepted as
    permanently fine -- distinct from KNOWN_PENDING above, which means "a
    decision is outstanding".  This means "reviewed, and it is legitimately
    fine, forever": a citation of a private artifact's name that reproduces
    none of its content (e.g. the CHANGELOG recording that a doc was
    relocated, or a historical cross-reference to its since-moved filename).

    Matched on `path` + `category` + `excerpt` (see _is_reviewed_accepted)
    -- deliberately NOT on `line`.  CHANGELOG.md line numbers drift as
    entries are prepended, so anchoring on a bare line number silently
    re-points the exemption at whatever unrelated content later occupies
    that line, exempting the wrong thing instead of failing loud (measured:
    one new top-of-file CHANGELOG.md entry shifted every entry below off
    its true line and turned all four back into findings).  `line` is kept
    only as a human-facing hint for where the reviewer originally found the
    text -- it plays no part in matching, and does not need to be updated
    when the file shifts.

    `excerpt` must be a substring of the matching line for the exemption to
    apply, AND it must match EXACTLY ONE line of the file it names --
    enforced by `scan()` itself (see the per-file `active_entries` filter
    there), not merely hoped for.  Matching by content with no positional
    bound would be unsafe on its own: the relocated AEN feature-coverage
    audit doc's bare filename, on its own, matches FOUR distinct
    CHANGELOG.md lines -- three of them the real PRIVATE_AUDIT_REFERENCE
    entries below -- not the one each entry was reviewed against, so an
    entry signed off on one specific sentence would silently exempt every
    other line citing that filename too, including a brand-new bullet no
    reviewer had ever seen.  "Long and specific" is not a bound; a repeated
    citation of the same artifact is exactly the shape this file
    legitimately has more than once.

    An entry whose excerpt matches nowhere at all in the file it names is a
    dead exemption -- the reviewed content was deleted, not just moved or
    reworded.  An entry whose excerpt matches more than one line is an
    ambiguous exemption -- content nobody reviewed would otherwise inherit
    the sign-off, silently, including future content.  `scan()` suppresses
    nothing for either case (the ordinary finding reappears on every
    matching line), and `_reviewed_accepted_integrity_messages`, called
    from `main()` after `scan()` returns -- never from inside `scan()`
    itself, so a stale or ambiguous entry can never discard the real
    findings the same run also collected -- prints why, whenever the run's
    root is this actual checkout.

    `reason` is required and non-empty (enforced by
    _validate_reviewed_accepted at import time) so nobody can land a silent
    exemption -- every entry below carries the reviewer's own words for why
    it is not a leak.
    """
    path: str
    line: int
    category: str
    excerpt: str
    reason: str


def _validate_reviewed_accepted(entries: tuple[ReviewedAcceptedEntry, ...]) -> None:
    for e in entries:
        if not e.reason or not e.reason.strip():
            raise SystemExit(
                f"check_public_private: REVIEWED_ACCEPTED entry "
                f"{e.path}:{e.line} {e.category} has no reason -- refusing "
                f"to start rather than silently exempt it."
            )
        if not e.excerpt or not e.excerpt.strip():
            raise SystemExit(
                f"check_public_private: REVIEWED_ACCEPTED entry "
                f"{e.path}:{e.line} {e.category} has no excerpt -- refusing "
                f"to start rather than anchor a permanent exemption on a "
                f"bare line number."
            )


# Findings a human reviewed and signed off as permanently fine.  Issue #524:
# these are the audit trail of a past privacy action (the CHANGELOG entries
# recording that two artifacts -- an Ensemble peripheral-coverage audit doc
# and a carrier errata note -- left the public tree) or a historical
# citation of the audit doc's since-relocated filename -- never a
# reproduction of the private content itself.  Issue #1515 adds one more:
# a docs/superpowers planning-doc line that names alp-sdk-internal as the
# destination for SoM-internal detail, itself describing the public/private
# boundary rather than disclosing anything across it.  Do not add an entry
# here to make a NEW finding go away; that is what this list is not for --
# take it to the maintainer first.
REVIEWED_ACCEPTED: tuple[ReviewedAcceptedEntry, ...] = (
    ReviewedAcceptedEntry(
        path="CHANGELOG.md",
        line=6899,
        category="SOM_PHYSICAL_DESIGN_DETAIL",
        # Split so this source line does not itself trip
        # SOM_PHYSICAL_DESIGN_DETAIL -- the runtime value is unchanged
        # (adjacent literals concatenate with no glue character); see the
        # NOTE ON FIXTURE STRINGS in tests/scripts/test_check_public_private.py.
        excerpt="see the internal" " carrier errata",
        reason=(
            "Names the private carrier-errata document as a pointer only, "
            "explaining why the loopback example taps the raw DAC0 net "
            "instead of the buffered path -- reproduces none of the "
            "errata's content."
        ),
    ),
    ReviewedAcceptedEntry(
        path="CHANGELOG.md",
        line=8157,
        category="PRIVATE_AUDIT_REFERENCE",
        # Lengthened to the surrounding sentence, not just the bare
        # filename: the filename alone matches four distinct CHANGELOG.md
        # lines (see the uniqueness bound in ReviewedAcceptedEntry's
        # docstring), so this must be long enough to be the ONLY line it
        # matches.  Split so this source line does not itself trip
        # PRIVATE_AUDIT_REFERENCE -- runtime value unchanged; see the NOTE
        # ON FIXTURE STRINGS in tests/scripts/test_check_public_private.py.
        excerpt="docs/aen-feature-au" "dit-2026-05.md` and the iMX93 reference",
        reason=(
            "Historical citation of the (since-relocated) audit filename, "
            "in the entry explaining why AEN/iMX93 SoM presets carry TBD "
            "hardware-fact fields.  Predates the relocation, so this "
            "paragraph carries no removed/moved-to language of its own for "
            "the PRIVATE_AUDIT_REFERENCE removal-description exemption to "
            "catch -- reviewed by hand instead.  No audit content "
            "reproduced, only the filename."
        ),
    ),
    ReviewedAcceptedEntry(
        path="CHANGELOG.md",
        line=10986,
        category="PRIVATE_AUDIT_REFERENCE",
        # Lengthened for the same uniqueness reason as the PRIVATE_AUDIT_REFERENCE entry above.
        excerpt="docs/aen-feature-au" "dit-2026-05.md` -- Alif Ensemble peripheral",
        reason=(
            "The CHANGELOG entry recording the audit doc's original "
            "addition to the tree (2026-05-14) -- a filename citation in "
            "the historical record of a real internal review that shaped a "
            "real public change.  Predates the relocation entry, so it "
            "carries no removed/moved-to language of its own.  No audit "
            "content reproduced."
        ),
    ),
    ReviewedAcceptedEntry(
        path="CHANGELOG.md",
        line=11148,
        category="PRIVATE_AUDIT_REFERENCE",
        # Lengthened for the same uniqueness reason as the PRIVATE_AUDIT_REFERENCE entry above.
        excerpt="docs/aen-feature-au" "dit-2026-05.md): customers migrating from",
        reason=(
            "Cites the audit filename as the source of the gpu2d.h "
            "portability gap it closed.  Predates the relocation entry, so "
            "it carries no removed/moved-to language of its own.  No audit "
            "content reproduced, only the filename."
        ),
    ),
    ReviewedAcceptedEntry(
        path="docs/superpowers/plans/2026-07-05-block-physical-layer.md",
        line=23,
        category="PRIVATE_DESIGN_REFERENCE",
        # Issue #1515: the one legitimate PRIVATE_DESIGN_REFERENCE hit
        # measured across all of docs/superpowers (98 files / 50,112
        # lines) -- SOM_PHYSICAL_DESIGN_DETAIL and PCB_ROUTING_DETAIL
        # measured zero there.  Split so this source line does not itself
        # trip PRIVATE_DESIGN_REFERENCE;
        # runtime value unchanged -- see the NOTE ON FIXTURE STRINGS in
        # tests/scripts/test_check_public_private.py.
        excerpt="mark `visibility: internal` and leave the detail-rich body for `alp-sdk-" "internal`",
        reason=(
            "This line is planning-doc guidance instructing authors to route "
            "SoM-internal detail to alp-sdk-internal and mark it "
            "visibility: internal -- it names the private repo as a "
            "destination, describing the public/private boundary itself, "
            "and reproduces no design/netlist/schematic content."
        ),
    ),
)

_validate_reviewed_accepted(REVIEWED_ACCEPTED)


def _is_reviewed_accepted(
    entries: Iterable[ReviewedAcceptedEntry], category: str, line_text: str
) -> bool:
    """`entries` is the caller's pre-filtered, per-file `active_entries` --
    see `scan()` -- already narrowed to this file's `path` AND to entries
    whose `excerpt` matches exactly one line of it.  This function only
    decides `category` + content; the uniqueness bound lives at the call
    site because it needs every line of the file, not just the one being
    tested."""
    return any(category == e.category and e.excerpt in line_text for e in entries)


def _reviewed_accepted_integrity_messages(root: Path, paths: Iterable[Path]) -> list[str]:
    """Return one diagnostic string per REVIEWED_ACCEPTED entry that is no
    longer safe to trust against real file content -- never raises, so a
    caller can print these ALONGSIDE a run's real findings instead of in
    place of them (see the call site in main()).

    Called from main() after scan() returns, not from inside scan() itself:
    a leak scanner that raised out of the scan loop the moment one exemption
    went stale would discard every genuine finding that run had already
    collected -- worse than the drift bug the exemption exists to tolerate.
    Two conditions, both worth a loud printed line instead of a silent
    pass:

      * stale (0 matches) -- the excerpt appears nowhere in the file any
        more.  The reviewed content was deleted, not just moved or
        reworded; the exemption exempts nothing and is dead weight.
      * ambiguous (>1 matches) -- the excerpt appears on more than one
        line.  scan()'s own per-file `active_entries` filter already
        refuses to suppress ANY of those lines for this reason (matching
        is by content, not position, so a non-unique excerpt would
        silently exempt lines nobody reviewed); this is the loud
        explanation for why those findings came back, not the mechanism
        that brings them back.

    Only ever meaningful for the actual repo checkout this script lives
    in -- REVIEWED_ACCEPTED's excerpts are a snapshot of alp-sdk's own
    CHANGELOG.md at specific points in its history, reviewed by a human
    against that specific content.  The caller (main()) only invokes this
    when `root == REPO`; a --root pointed at a historical branch, a
    release worktree, a fork, or an unrelated clean tree has no
    relationship to these entries and must not be asked to satisfy them:
    without this scoping, a private-clean tree under a different --root
    would exit 1 for a reason having nothing to do with its own content.

    Not run at import time alongside _validate_reviewed_accepted, and not
    because there is no tree to read yet -- REPO is a module-level constant,
    available at import.  The real constraint is that this module is
    imported directly by the test suite (`import check_public_private as
    classifier`), before any test has written its tmp_path fixtures --
    reading the real repo's CHANGELOG.md at import time would pull live
    content into every test process instead of leaving each test's fixture
    tree isolated, and would run before argparse has resolved which root
    and which paths this particular invocation actually wants checked.
    """
    scanned_rels = {_rel(p, root) for p in paths}
    messages: list[str] = []
    for e in REVIEWED_ACCEPTED:
        if e.path not in scanned_rels:
            continue
        try:
            text = (root / e.path).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        match_count = sum(1 for line in text.splitlines() if e.excerpt in line)
        if match_count == 0:
            # No ":{e.line}" here -- `line` is a human hint only (see the
            # ReviewedAcceptedEntry docstring), not maintained against file
            # drift, so rendering it as a file:line locator would send a
            # reader to unrelated content.
            messages.append(
                f"check_public_private: REVIEWED_ACCEPTED entry for "
                f"{e.path} ({e.category}) has a stale excerpt -- "
                f"{e.excerpt!r} no longer appears anywhere in {e.path}. "
                f"The content it was reviewed against was deleted or "
                f"reworded away; remove this entry or re-derive `excerpt` "
                f"from the current text."
            )
        elif match_count > 1:
            messages.append(
                f"check_public_private: REVIEWED_ACCEPTED entry for "
                f"{e.path} ({e.category}) has an ambiguous excerpt -- "
                f"{e.excerpt!r} matches {match_count} lines in {e.path}, "
                f"not exactly one, so it exempts none of them.  Tighten "
                f"`excerpt` to a substring unique to the reviewed line; if "
                f"the colliding lines are byte-identical, no substring can "
                f"be unique -- edit one of the lines' surrounding text "
                f"instead so they no longer read the same."
            )
    return messages


# Phrases that, appearing anywhere in the bullet/paragraph a
# PRIVATE_AUDIT_REFERENCE match sits in, mark that paragraph as *describing*
# a removal/relocation rather than disclosing content -- e.g. "removed from
# the public SDK ... Moved to alplabai/e1m-som-metadata".
_REMOVAL_PHRASE = re.compile(r"removed from the public|\bmoved to\b", re.IGNORECASE)


def _block_range(lines: list[str], idx: int) -> tuple[int, int]:
    """0-indexed [start, end] of the CHANGELOG-style bullet/paragraph
    containing lines[idx].

    CHANGELOG.md entries are markdown list items: a line starting with
    "- " at column 0 opens one, 2-space-indented lines continue it, and a
    blank line or the next "- " ends it.  This is deliberately not a
    general markdown parser -- it exists only to scope the narrow
    removal-description check below to the one paragraph a match sits in,
    so it can never reach into an unrelated bullet earlier or later in the
    file.
    """
    if lines[idx].startswith("- "):
        start = idx
    else:
        start = idx
        while start > 0:
            start -= 1
            if lines[start].startswith("- "):
                break
            if lines[start].strip() == "":
                start += 1
                break
        else:
            start = 0
    end = start
    while end + 1 < len(lines):
        nxt = lines[end + 1]
        if nxt.strip() == "" or nxt.startswith("- "):
            break
        end += 1
    return start, end


def _describes_removal(lines: list[str], line_no: int) -> bool:
    """True if the paragraph containing 1-indexed line_no itself records
    that the cited artifact left the public repo (see _REMOVAL_PHRASE) --
    e.g. the CHANGELOG entry documenting a scrub.  Only ever consulted for
    rules with exempt_if_describes_removal=True (PRIVATE_AUDIT_REFERENCE):
    it suppresses a citation of a filename/phrase inside the very paragraph
    that says that file left the public tree, never a reproduction of
    content."""
    start, end = _block_range(lines, line_no - 1)
    return bool(_REMOVAL_PHRASE.search("\n".join(lines[start:end + 1])))


RULES: tuple[Rule, ...] = (
    Rule(
        "LOCAL_MAINTAINER_PATH",
        re.compile(
            r"(?:/home/" r"caner(?:/|\b)|C:\\Users\\" r"Caner(?:\\|\b))"
        ),
        "Use a placeholder such as <repo>, <ti-sdk>, or derive the path from the script location.",
    ),
    Rule(
        "PRIVATE_AUDIT_REFERENCE",
        re.compile(
            r"(?:internal\s+AEN\s+feature\s+audit|AEN-" r"FEATURE-AUDIT|"
            r"private\s+repo[^.\n]{0,80}\breport\b|"
            r"\breport\b[^.\n]{0,80}private\s+repo)",
            re.IGNORECASE,
        ),
        "Replace private audit/report citations with public SDK support docs or neutral rationale.",
        exempt_if_describes_removal=True,
    ),
    Rule(
        "PRIVATE_DESIGN_REFERENCE",
        re.compile(
            r"(?:(?:alp-sdk-internal|private\s+repo|privately)[^.\n]{0,100}"
            r"\b(?:netlist|schematic|design|audit|errata|rework|report)\b|"
            r"\b(?:netlist|schematic|design|audit|errata|rework|report)\b"
            r"[^.\n]{0,100}(?:alp-sdk-internal|private\s+repo|privately))",
            re.IGNORECASE,
        ),
        "Keep private design/archive locations out of public text; describe only the public contract.",
    ),
    Rule(
        "SOM_PHYSICAL_DESIGN_DETAIL",
        re.compile(
            r"(?:\bSoC\s+balls?\b|\bSoM\s+test\s+points?\b|"
            r"metadata/e1m_modules/[^`'\" \t\n]*/netlists|"
            r"\bNEXT-REV\s+SoM\s+netlist\b|"
            r"\bBuilt\s+from\b[^.\n]{0,80}\bnetlist\b|"
            r"\binternal\s+carrier\s+errata\b|"
            r"\bbench\s+rework\b)",
            re.IGNORECASE,
        ),
        "Remove schematic-level SoM implementation detail from public text.",
    ),
    Rule(
        "DANGLING_PRIVATE_NOTES_LINK",
        # `memory/<project|feedback|reference>-<slug>.md` is the naming
        # convention for the maintainer's local AI-memory store (see
        # MEMORY.md in the user's Claude config) -- it has never existed in
        # this repo, so any such path cited in checked-in text is a dead
        # citation for every other clone.  Deliberately literal rather than
        # "any relative markdown link that doesn't resolve to a tracked
        # file": that general form needs real link resolution (relative-path
        # math, git-ls-files lookups, code-fence awareness) to avoid its own
        # false positives, is a bigger surface than this scrub calls for, and
        # the naming convention alone already caught every live instance in
        # this repo (verified against a full-repo grep before landing this
        # rule) -- widen to the general form the day a second naming
        # convention actually leaks past it.
        re.compile(r"\bmemory/(?:project|feedback|reference)[-_][A-Za-z0-9_-]*\.md\b"),
        "Drop the private-notes citation; keep the technical statement it was attached to on its own.",
    ),
    Rule(
        "LABGRID_PLACE",
        re.compile(r"labgrid " r"place ", re.IGNORECASE),
        "Drop the internal labgrid-place identifier; keep the bench-proven claim and its date.",
    ),
    Rule(
        "PROBE_SERIAL",
        re.compile(
            r"(?:J-Link\b[^.\n]{0,80}\bS/N\s*\d+|\bS/N\s*\d{5,}\b)",
            re.IGNORECASE,
        ),
        "Drop the probe serial number; keep the troubleshooting point it illustrates.",
    ),
    Rule(
        "LAB_SSH_ENDPOINT",
        # A literal dotted-quad after `root@` is a real, reachable bench/lab
        # host -- unlike `root@<board>` or `root@e1m-v2n101-a55.local`,
        # which are customer-facing placeholder / mDNS forms and stay clean.
        re.compile(r"\broot@\d{1,3}(?:\.\d{1,3}){3}\b"),
        "Generalise to a placeholder (e.g. root@<bench-host>); do not substitute a different real address.",
    ),
    Rule(
        "LAB_INFRA_HOSTNAME",
        # `<label>.alplab.ai` where <label> isn't one of the customer-facing
        # subdomains this repo already publishes (docs.alplab.ai,
        # community.alplab.ai) or the `*.example.alplab.ai` placeholder used
        # in example code (examples/connectivity/*) -- verified against every
        # alplab.ai mention in the tree before this rule landed.  A new,
        # unlisted label is exactly the shape issue #524 found: `erp.alplab
        # .ai`, an internal ops host (Hostinger VPS), sitting in a design
        # spec.  Widen the allowlist the day a legitimate new public
        # subdomain trips this.
        re.compile(
            r"\b(?!(?:www|community|docs|status|forum|blog|example)\.alplab\.ai\b)"
            r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.alplab\.ai\b",
            re.IGNORECASE,
        ),
        "Generalise the internal host to a placeholder (e.g. <ops-host>.alplab.ai) or drop it.",
    ),
    Rule(
        "LAB_INFRA_IP",
        # A backtick-quoted (Markdown code-span) IPv4 literal.  Anchored to
        # backticks specifically because this tree's spec/clause citations
        # (`IEEE 802.3 clause 22.2.4.1`, `HWRM section 14.10.5.3`) and
        # dotted version strings (`0.4.9.1`) are dotted-quad-shaped but are
        # never backtick-quoted -- verified against every IPv4-shaped
        # literal in the tree before this rule landed; revisit if a future
        # backtick'd spec/version number ever trips it.  Reserved/private/
        # loopback/link-local/documentation ranges (RFC 1918, RFC 5737) are
        # excluded because this repo legitimately cites those in bench and
        # driver text (`192.168.10.137` DHCP leases, `192.0.2.0/24` docs
        # examples).  `93.184.216.34` -- example.com's long-documented public
        # IP, used in two examples as a stable public TCP-reachability
        # target -- is excluded by name for the same reason: a real,
        # intentional, non-lab public address.
        re.compile(
            r"`(?!0\.|10\.|127\.|169\.254\.|172\.(?:1[6-9]|2\d|3[01])\.|"
            r"192\.168\.|192\.0\.2\.|198\.51\.100\.|203\.0\.113\.|"
            r"(?:22[4-9]|2[3-4]\d|25[0-5])\.|93\.184\.216\.34\b)"
            r"(?:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.){3}"
            r"(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)`"
        ),
        "Generalise the address to a placeholder; do not substitute a different real one.",
    ),
    Rule(
        "PCB_ROUTING_DETAIL",
        # Phrase-keyed, not numeric: a bare "22 ohm" or "5 mm" is often a
        # legitimate customer-facing driver fact (a real shunt value, a
        # cable-length limit); these five phrases are PCB-layout vocabulary
        # that only shows up when describing the physical routing itself.
        re.compile(
            r"(?:\blength\s+matching\b|\bBGA\s+designators?\b|"
            r"\bseries\s+resistor\s+IDs?\b|\blayer\s+stackup\b|"
            r"\bdifferential\s+pair\s+length\b)",
            re.IGNORECASE,
        ),
        "Keep PCB-routing/impedance/BGA-designator implementation detail out of public text.",
    ),
)


def _rel(path: Path, base: Path) -> str:
    try:
        return path.relative_to(base).as_posix()
    except ValueError:
        return path.as_posix()


def _is_excluded(rel: str, excludes: Iterable[str] = DEFAULT_EXCLUDES) -> bool:
    parts = rel.split("/")
    for excl in excludes:
        excl_parts = excl.split("/")
        if parts[:len(excl_parts)] == excl_parts:
            return True
    return False


def _is_text_path(path: Path) -> bool:
    return path.name in TEXT_FILENAMES or path.suffix in TEXT_SUFFIXES


def _git_ls_files(root: Path) -> list[Path] | None:
    try:
        proc = subprocess.run(
            ["git", "-C", str(root), "ls-files"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return [root / line for line in proc.stdout.splitlines() if line]


def discover_files(root: Path, roots: Iterable[str] = DEFAULT_ROOTS) -> list[Path]:
    root = root.resolve()
    wanted = tuple(Path(r) for r in roots)
    git_files = _git_ls_files(root)
    if git_files is None:
        candidates: list[Path] = []
        for rel_root in wanted:
            p = root / rel_root
            if p.is_file():
                candidates.append(p)
            elif p.is_dir():
                candidates.extend(x for x in p.rglob("*") if x.is_file())
    else:
        candidates = []
        for path in git_files:
            rel_path = path.relative_to(root)
            if any(rel_path == w or rel_path.is_relative_to(w) for w in wanted):
                candidates.append(path)

    out: list[Path] = []
    for path in candidates:
        rel = _rel(path, root)
        if _is_excluded(rel) or not _is_text_path(path):
            continue
        out.append(path)
    return sorted(out)


def scan(paths: Iterable[Path], *, base: Path) -> list[Finding]:
    findings: list[Finding] = []
    for path in paths:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        rel = _rel(path, base)
        lines = text.splitlines()
        # A REVIEWED_ACCEPTED entry only suppresses a finding when its
        # excerpt matches EXACTLY ONE line of this file -- see the
        # uniqueness bound documented on ReviewedAcceptedEntry.  Zero
        # matches (stale) or more than one (ambiguous) both leave the
        # entry out of `active_entries` entirely, so the ordinary finding
        # goes through unsuppressed on every matching line;
        # _reviewed_accepted_integrity_messages explains why, separately --
        # but only when the run's root is this actual checkout (root ==
        # REPO in main()).  This filter itself has no such guard and runs
        # for every --root: a stale/ambiguous entry still fails closed
        # under a foreign tree (the finding surfaces), it just does so
        # without the printed explanation.  That is intentional, not an
        # oversight -- REVIEWED_ACCEPTED's excerpts are a snapshot of
        # alp-sdk's own CHANGELOG.md and say nothing true about someone
        # else's tree, so the diagnostic is scoped to root == REPO on
        # purpose.  It can only matter in practice if a foreign tree has a
        # file at the same relative `path` whose content happens to
        # collide with one of these excerpts -- vanishingly unlikely for a
        # private-design/maintainer-path/audit-reference phrase this
        # specific.
        active_entries = [
            e for e in REVIEWED_ACCEPTED
            if e.path == rel and sum(1 for ln in lines if e.excerpt in ln) == 1
        ]
        for line_no, line in enumerate(lines, start=1):
            for rule in RULES:
                match = rule.pattern.search(line)
                if not match:
                    continue
                if _is_known_pending(rel, rule.category, line_no):
                    continue
                if _is_reviewed_accepted(active_entries, rule.category, line):
                    continue
                if rule.exempt_if_describes_removal and _describes_removal(lines, line_no):
                    continue
                findings.append(Finding(
                    path=rel,
                    line=line_no,
                    category=rule.category,
                    matched_text=match.group(0),
                    suggestion=rule.suggestion,
                ))
    return findings


def _format_finding(f: Finding) -> str:
    return (f"{f.path}:{f.line} {f.category}: `{f.matched_text}` -- "
            f"{f.suggestion}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=REPO,
                        help="Repository root (default: this checkout)")
    parser.add_argument("--path", action="append", default=[],
                        help="Specific file or directory to scan; repeatable")
    parser.add_argument("--json", action="store_true",
                        help="Emit one JSON object per finding")
    parser.add_argument("--quiet", action="store_true",
                        help="Suppress per-finding text output")
    args = parser.parse_args(argv)

    root = args.root.resolve()
    if not root.exists():
        print(f"check_public_private: root does not exist: {root}", file=sys.stderr)
        return 2

    if args.path:
        paths: list[Path] = []
        for raw in args.path:
            p = Path(raw)
            if not p.is_absolute():
                p = root / p
            if p.is_file():
                paths.append(p)
            elif p.is_dir():
                paths.extend(x for x in p.rglob("*") if x.is_file())
            else:
                print(f"check_public_private: path does not exist: {raw}", file=sys.stderr)
                return 2
        paths = [p for p in paths if not _is_excluded(_rel(p, root)) and _is_text_path(p)]
    else:
        paths = discover_files(root)

    findings = scan(paths, base=root)

    # REVIEWED_ACCEPTED's excerpts are a snapshot of THIS repo's own
    # CHANGELOG.md content, reviewed by a human against specific text at
    # specific points in alp-sdk's history -- meaningless against a --root
    # pointed anywhere else (a historical branch, a release worktree, a
    # fork, an unrelated clean tree).  Only ever run against the actual
    # checkout this script lives in.
    integrity_messages: list[str] = (
        _reviewed_accepted_integrity_messages(root, paths) if root == REPO else []
    )

    if args.json:
        for finding in findings:
            print(json.dumps(asdict(finding), sort_keys=True))
    elif findings and not args.quiet:
        for finding in findings:
            print(_format_finding(finding))

    # Printed regardless of --json/--quiet: these are gate-integrity
    # failures (a stale or ambiguous exemption), not findings, and must
    # never be silently dropped alongside -- or instead of -- real ones.
    for msg in integrity_messages:
        print(msg, file=sys.stderr)

    if findings or integrity_messages:
        if not args.json and findings:
            plural = "" if len(findings) == 1 else "s"
            print(f"public-private: {len(findings)} finding{plural} -- failing.",
                  file=sys.stderr)
        return 1

    if not args.json:
        print("public-private: OK (no private design references or maintainer-local paths).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
